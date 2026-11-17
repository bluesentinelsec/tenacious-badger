"""Fail when C code uses the C library where the foundation layer has an equivalent.

The foundation modules exist so that the engine has one implementation of
allocation, strings, formatted output, assertions, and the platform services:
one place that counts, checks, and reports, and one behavior on every
platform. That only holds while code uses them. This check reads every C
source and header in the directories that cmake/FormatSources.cmake names, the
same files that clang-format maintains, and reports any use of a C library or
operating system function, type, or header that a foundation module replaces,
together with what to use instead.

A function with no equivalent in the foundation is not reported: memcpy,
strtoul, and qsort are examples today. When a module gains the equivalent, its
rule is added to RULES below in the same change.

A file that legitimately needs a replaced function is authorized by name in
AUTHORIZED below, with the reason. There is no way to suppress the check from
inside a source file.

  python tools/check_foundation.py              Check the project.
  python tools/check_foundation.py --list       Print every rule.
  python tools/check_foundation.py --self-test  Check this script.
"""
import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
SOURCE_LIST = ROOT / 'cmake/FormatSources.cmake'


def rule(module, document, replacements):
    """One foundation module's rule: each replaced name, and what replaces it.

    `replacements` maps what to use to the names it replaces, separated by
    spaces. A name in angle brackets is a header. A name that ends in `*`
    stands for every identifier that begins with the part before it.
    """
    return {name: (replacement, module, document)
            for replacement, names in replacements.items() for name in names.split()}


ALLOCATION = rule('badger/memory.h', 'docs/memory.md', {
    'tb_malloc, tb_calloc, tb_realloc, or tb_free':
        'malloc calloc realloc free reallocarray _recalloc _expand',
    'tb_str_dup or tb_str_sub':
        'strdup _strdup strndup',
    'tb_fmt_string or tb_fmt_vstring':
        'asprintf vasprintf',
    'a copy made into memory from tb_malloc':
        'wcsdup _wcsdup _mbsdup',
    'a fixed-size array, or memory from tb_malloc':
        'alloca _alloca _malloca _freea <alloca.h>',
    'tb_aligned_alloc and tb_aligned_free':
        'aligned_alloc posix_memalign memalign valloc pvalloc _aligned_malloc _aligned_realloc '
        '_aligned_recalloc _aligned_free',
    'the engine allocation functions, or a platform memory interface':
        'HeapAlloc HeapReAlloc HeapFree HeapCreate',
    'tb_vm_reserve, tb_vm_commit, tb_vm_decommit, and tb_vm_release':
        'VirtualAlloc VirtualFree mmap munmap mprotect',
    'badger/memory.h':
        '<malloc.h>',
})

FORMATTING = rule('badger/fmt.h', 'docs/strings.md', {
    'tb_fmt_print or tb_fmt_print_error':
        'printf vprintf puts putchar perror printf_s vprintf_s',
    'tb_fmt_print, tb_fmt_print_error, or tb_fmt_write to a destination of your own':
        'fprintf vfprintf fputs fputc putc dprintf vdprintf fprintf_s vfprintf_s',
    'tb_fmt_buffer or tb_fmt_string':
        'sprintf snprintf vsprintf vsnprintf _snprintf _vsnprintf sprintf_s snprintf_s '
        'vsprintf_s vsnprintf_s _snprintf_s _vsnprintf_s',
    'tb_fmt_flush, which reaches the streams that tb_fmt_print writes to':
        'fflush',
})

STRINGS = rule('badger/str.h', 'docs/strings.md', {
    'tb_str_length':
        'strlen',
    'tb_str_equal, tb_str_compare, tb_str_starts_with, or tb_str_matches_at':
        'strcmp strncmp',
    'tb_str_find_char or tb_str_rfind_char':
        'strchr strrchr',
    'tb_str_find or tb_str_rfind':
        'strstr',
    'tb_str_find_any, tb_str_span, or their reverse forms':
        'strpbrk strcspn strspn',
    'tb_str_dup, tb_str_sub, tb_str_cat, or tb_str_join; tb_fmt_buffer for a buffer of your own':
        'strcpy strncpy strcat strncat strlcpy strlcat stpcpy stpncpy strcpy_s strncpy_s '
        'strcat_s strncat_s',
    'tb_str_find_any with tb_str_sub, or badger/text.h for parts that are not copied':
        'strtok strtok_r strtok_s strsep',
})

FAILURES = rule('badger/assert.h', 'include/badger/assert.h', {
    'TB_ASSERT or TB_ASSERT_MSG':
        'assert _ASSERT _ASSERTE <assert.h>',
    'TB_STATIC_ASSERT':
        'static_assert _Static_assert',
    'TB_ASSERT for a broken contract, or tb_platform_abort after reporting a failure':
        'abort',
})

TIMING = rule('badger/platform.h', 'docs/platforms.md', {
    'tb_platform_clock_nanoseconds':
        'timespec_get clock_gettime clock_gettime_nsec_np gettimeofday '
        'QueryPerformanceCounter QueryPerformanceFrequency GetTickCount GetTickCount64 '
        'mach_absolute_time mach_continuous_time',
    'tb_platform_sleep, or tb_platform_wait_until to wait for a moment':
        'Sleep SleepEx nanosleep clock_nanosleep usleep CreateWaitableTimerW '
        'CreateWaitableTimerExW SetWaitableTimer mach_wait_until',
})

ATOMICS = rule('badger/platform.h', 'docs/platforms.md', {
    'tb_platform_atomic_size, tb_platform_atomic_u32, or tb_platform_atomic_pointer':
        '_Atomic <stdatomic.h> Interlocked* _Interlocked* __atomic_* __sync_*',
})

THREADS = rule('badger/thread.h', 'docs/platforms.md', {
    'tb_thread_create, tb_thread_join, and tb_thread_current_id':
        'thrd_* CreateThread _beginthread _beginthreadex GetCurrentThreadId',
    'tb_thread_create_with_options and tb_thread_current_name':
        'SetThreadDescription GetThreadDescription SetThreadPriority prctl',
    'tb_mutex and its functions':
        'mtx_* AcquireSRWLockExclusive ReleaseSRWLockExclusive TryAcquireSRWLockExclusive '
        'AcquireSRWLockShared ReleaseSRWLockShared TryAcquireSRWLockShared '
        'InitializeCriticalSection EnterCriticalSection TryEnterCriticalSection '
        'LeaveCriticalSection DeleteCriticalSection',
    'tb_condition and its functions':
        'cnd_* SleepConditionVariableSRW SleepConditionVariableCS WakeConditionVariable '
        'WakeAllConditionVariable',
    'the threads, mutexes, and condition variables of badger/thread.h':
        '<threads.h> <pthread.h> pthread_*',
    'the C keyword _Thread_local, or tb_thread_local for a place made at run time':
        'thread_local __thread tss_* TlsAlloc TlsGetValue TlsSetValue TlsFree '
        'FlsAlloc FlsGetValue FlsSetValue FlsFree',
    'tb_semaphore and its functions':
        '<semaphore.h> sem_* CreateSemaphoreW CreateSemaphoreExW ReleaseSemaphore '
        'dispatch_semaphore_*',
    'tb_thread_relax in a loop that waits, and tb_thread_yield to give up a turn':
        '_mm_pause YieldProcessor __builtin_ia32_pause sched_yield SwitchToThread',
})

PROCESSORS = rule('badger/system.h', 'docs/platforms.md', {
    'tb_system_get_processors':
        'GetSystemInfo GetNativeSystemInfo GetLogicalProcessorInformation '
        'GetLogicalProcessorInformationEx GetActiveProcessorCount GetProcessAffinityMask '
        'GetProcessGroupAffinity sched_getaffinity get_nprocs get_nprocs_conf '
        '_SC_NPROCESSORS_ONLN _SC_NPROCESSORS_CONF',
})

COLLECTIONS = rule('badger/table.h', 'docs/collections.md', {
    'tb_table, tb_set, or tb_list':
        '<search.h>',
})

FILES = rule('badger/file.h', 'docs/files.md', {
    'tb_file_open, tb_file_close, tb_file_read, and tb_file_write':
        'fopen fopen_s fclose fread fwrite open close read write CreateFileW CreateFileA '
        'ReadFile WriteFile',
    'tb_file_seek, tb_file_tell, tb_file_size, and tb_file_flush':
        'fseek fseeko ftell ftello lseek fsync SetFilePointerEx GetFileSizeEx FlushFileBuffers',
    'tb_fs_query, tb_fs_make_directory, tb_fs_remove, and tb_fs_rename':
        'lstat fstat mkdir rmdir unlink remove rename GetFileAttributesW GetFileAttributesExW '
        'CreateDirectoryW RemoveDirectoryW DeleteFileW MoveFileExW SetFileInformationByHandle',
    'tb_directory_open, tb_directory_next, and tb_directory_close':
        'opendir readdir closedir FindFirstFileW FindNextFileW FindClose',
})

RULES = [ALLOCATION, FORMATTING, STRINGS, FAILURES, TIMING, ATOMICS, THREADS, PROCESSORS,
         COLLECTIONS, FILES]
FORBIDDEN = {name: entry for group in RULES for name, entry in group.items()}
PREFIXES = sorted((name[:-1] for name in FORBIDDEN if name.endswith('*')), key=len, reverse=True)

# Where a rule does not apply, and why. A path that ends in `/` is a directory
# and covers every file below it. Keep this list short: each entry is a place
# where the engine does not use its own foundation.
AUTHORIZED = {
    'src/platform/win32/virtual_memory.c': (
        {'VirtualAlloc', 'VirtualFree'}, 'implements the virtual memory interface'),
    'src/platform/posix/virtual_memory.c': (
        {'mmap', 'munmap', 'mprotect'}, 'implements the virtual memory interface'),
    'src/platform/linux/virtual_memory.c': (
        {'mmap'}, 'implements optional native large pages'),
    'src/platform/macos/virtual_memory.c': (
        {'mmap'}, 'implements optional native large pages'),
    # Native file adapters also build independently of the core allocator and
    # strings. Their small path/iterator allocations return resource errors.
    **{path: (set(names.split()),
              'implements native files beneath the foundation modules')
       for path, names in {
           'src/platform/shared/fs.c': 'malloc free strlen snprintf',
           'src/platform/posix/fs.c': 'malloc free strlen strcmp',
           'src/platform/win32/fs.c': 'malloc free',
           'src/platform/linux/fs_location.c': 'malloc free strlen',
           'src/platform/macos/fs_location.c': 'malloc free strlen',
       }.items()},

    # The modules that implement the replacements.
    'src/core/memory.c': (
        {'malloc', 'calloc', 'realloc', 'free'},
        'implements the engine allocation functions on top of the C library'),
    'src/core/fmt.c': (
        {'fflush', 'fwrite'},
        'implements badger/fmt.h on top of the C library\'s streams'),
    'src/core/str.c': (
        {'strlen', 'strcmp', 'strncmp'},
        'implements badger/str.h on top of the C library'),
    'include/badger/assert.h': (
        {'_Static_assert'},
        'defines TB_STATIC_ASSERT'),
    'src/platform/': (
        set(TIMING) | set(ATOMICS) | set(THREADS) | set(PROCESSORS) | set(FILES) | {'abort', 'fputs'},
        'implements the platform interfaces beneath the foundation modules, and is built '
        'and tested without them'),

    # Benchmarks that measure the C library beside the module that replaces it.
    'benchmarks/core/memory_bench.c': (
        {'malloc', 'free'},
        'measures the C library beside badger/memory.h'),
    'benchmarks/core/fmt_bench.c': (
        {'snprintf'},
        'measures the C library beside badger/fmt.h'),
    'benchmarks/core/text_bench.c': (
        {'strstr', 'strrchr', 'strcspn'},
        'measures the C library beside badger/str.h'),

    'src/platform/win32/system.c': (
        {'HeapAlloc', 'HeapFree'},
        'holds the system\'s description of the processors, whose size the system decides, '
        'beneath the engine\'s allocation functions'),

    # The platform adapters' tests are built without the foundation modules.
    'tests/support/platform_assert.c': (
        {'snprintf'},
        'reports an adapter\'s failed assertion to the test framework, in executables built '
        'without badger/fmt.h'),

    'tests/platform/stack_test.c': (
        {'strlen', 'strcmp', 'strncmp'},
        'compares the names that an adapter gives to addresses, in an executable built '
        'without badger/str.h'),
    'src/platform/posix/stack.c': (
        {'strlen'},
        'copies the names that the system gives to addresses, beneath badger/str.h'),

    # Tests that confine the process, to see the count of processors follow.
    'tests/platform/win32/affinity_test.c': (
        {'GetProcessAffinityMask'},
        'reads the processors of the process to confine it and to give them back'),
    'tests/platform/linux/affinity_test.c': (
        {'sched_getaffinity'},
        'reads the processors of the process to confine it and to give them back'),

    # Tests that check a module against the C library, which shares no code
    # with it.
    'tests/core/fmt_test.c': (
        {'snprintf'},
        'checks badger/fmt.h against the C library'),
    'tests/core/str_test.c': (
        {'strlen'},
        'checks badger/str.h against the C library'),
    'tests/core/text_test.c': (
        {'strlen'},
        'checks badger/text.h against the C library'),
    'tests/platform/time_test.c': (
        {'timespec_get'},
        'checks the engine clock against the C library'),

    # The test framework cannot link the engine, so it cannot use the
    # foundation. Its allocations stay confined to one file.
    'tests/framework/': (
        set(FORMATTING) | set(STRINGS) | set(FAILURES) | set(TIMING),
        'the test framework is independent of the engine it tests'),
    'tests/framework/src/support.c': (
        {'calloc', 'free'},
        'the test framework is independent of the engine it tests'),
    'tests/framework/src/platform/posix.c': (
        {'write'},
        'the crash handler needs async-signal-safe output without engine dependencies'),
    'tests/framework/src/platform/win32.c': (
        {'WriteFile'},
        'the crash handler writes prepared bytes without the CRT or engine dependencies'),
}

IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
SYSTEM_INCLUDE = re.compile(r'\s*#\s*include\s*<([^>]*)>')


def blank_comments_and_literals(text):
    """Replace comments and string/character literals with spaces.

    Newlines are kept, so line numbers in the result match the source.
    """
    result = []
    index = 0
    length = len(text)
    while index < length:
        character = text[index]
        following = text[index + 1] if index + 1 < length else ''
        if character == '/' and following == '/':
            end = text.find('\n', index)
            end = length if end == -1 else end
            # A line comment continues after a backslash-newline.
            while end < length and text[end - 1] == '\\':
                next_end = text.find('\n', end + 1)
                end = length if next_end == -1 else next_end
            result.append(re.sub(r'[^\n]', ' ', text[index:end]))
            index = end
        elif character == '/' and following == '*':
            end = text.find('*/', index + 2)
            end = length if end == -1 else end + 2
            result.append(re.sub(r'[^\n]', ' ', text[index:end]))
            index = end
        elif character in '"\'':
            end = index + 1
            while end < length and text[end] != character and text[end] != '\n':
                end += 2 if text[end] == '\\' else 1
            end = min(end + 1, length)
            result.append(re.sub(r'[^\n]', ' ', text[index:end]))
            index = end
        else:
            result.append(character)
            index += 1
    return ''.join(result)


def rule_name_of(identifier):
    """The name in FORBIDDEN that covers an identifier, or None."""
    if identifier in FORBIDDEN:
        return identifier
    for prefix in PREFIXES:
        if identifier.startswith(prefix):
            return prefix + '*'
    return None


def find_uses(text):
    """Return (line, written name, rule name) for each use of a replaced name.

    A header named by an include is one use, written with its angle brackets;
    the words inside the brackets are not identifiers.
    """
    uses = []
    for number, line in enumerate(blank_comments_and_literals(text).split('\n'), start=1):
        include = SYSTEM_INCLUDE.match(line)
        if include:
            header = f'<{include.group(1).strip()}>'
            if header in FORBIDDEN:
                uses.append((number, header, header))
            continue
        for match in IDENTIFIER.finditer(line):
            rule_name = rule_name_of(match.group(0))
            # POSIX uses ordinary English names that are also legitimate
            # local variables. Check calls for those ambiguous names.
            if rule_name in {'open', 'close', 'read', 'write', 'remove', 'rename'}:
                if not line[match.end():].lstrip().startswith('('):
                    continue
            if rule_name is not None:
                uses.append((number, match.group(0), rule_name))
    return uses


def authorized_names(path_name):
    """The rule names a file may use: its own entry and those of its directories."""
    names = set()
    for authorized_path, (authorized, _) in AUTHORIZED.items():
        is_directory = authorized_path.endswith('/')
        if path_name == authorized_path or (is_directory and path_name.startswith(authorized_path)):
            names |= authorized
    return names


def find_violations(path_name, text):
    """Return (line, written name, replacement, document) for each unauthorized use."""
    allowed = authorized_names(path_name)
    violations = []
    for number, written, rule_name in find_uses(text):
        if rule_name not in allowed:
            replacement, _, document = FORBIDDEN[rule_name]
            violations.append((number, written, replacement, document))
    return violations


def find_stale_authorizations(texts):
    """Return a message for each entry of AUTHORIZED that no longer earns its place.

    `texts` maps each project source to its text. An entry for a file must
    name only rules, and only ones the file still uses, so that an exception
    ends when its reason does. An entry for a directory must still hold
    sources.
    """
    messages = []
    for authorized_path, (authorized, _) in AUTHORIZED.items():
        for name in sorted(authorized - set(FORBIDDEN)):
            messages.append(f'{authorized_path} is authorized for {name}, which no rule names; '
                            'remove it')
        if authorized_path.endswith('/'):
            if not any(path_name.startswith(authorized_path) for path_name in texts):
                messages.append(f'authorized directory {authorized_path} holds no sources in '
                                f'{SOURCE_LIST.name}; remove its entry')
            continue
        if authorized_path not in texts:
            messages.append(f'authorized file {authorized_path} is not in {SOURCE_LIST.name}; '
                            'remove its entry')
            continue
        used = {rule_name for _, _, rule_name in find_uses(texts[authorized_path])}
        for name in sorted((authorized & set(FORBIDDEN)) - used):
            messages.append(f'{authorized_path} is authorized for {name} but does not use it; '
                            'remove the authorization')
    return messages


def read_setting(listing, name):
    """The words of `set(<name> word...)` in the text of a CMake file, or None."""
    match = re.search(rf'^set\({name}((?:\s+[\w.*/-]+)+)\s*\)\s*$', listing, flags=re.MULTILINE)
    return match.group(1).split() if match else None


def project_sources():
    """The C sources and headers of the project, as cmake/FormatSources.cmake finds them.

    That file names the directories to search and the file name patterns to
    match. Reading both from it keeps this check and clang-format on the same
    files.
    """
    listing = SOURCE_LIST.read_text(encoding='utf-8')
    directories = read_setting(listing, 'TB_FORMAT_DIRECTORIES')
    patterns = read_setting(listing, 'TB_FORMAT_PATTERNS')
    if not directories or not patterns:
        sys.exit(f'{SOURCE_LIST}: TB_FORMAT_DIRECTORIES or TB_FORMAT_PATTERNS not found; '
                 'has its format changed?')
    sources = set()
    for directory in directories:
        for pattern in patterns:
            sources.update(path.relative_to(ROOT).as_posix()
                           for path in (ROOT / directory).rglob(pattern) if path.is_file())
    if not sources:
        sys.exit(f'{SOURCE_LIST}: no sources found in {", ".join(directories)}')
    return sorted(sources)


def check_project():
    texts = {path_name: (ROOT / path_name).read_text(encoding='utf-8')
             for path_name in project_sources()}
    problems = 0
    for message in find_stale_authorizations(texts):
        print(f'{Path(__file__).name}: error: {message}')
        problems += 1
    for path_name, text in texts.items():
        for number, written, replacement, document in find_violations(path_name, text):
            print(f'{ROOT / path_name}:{number}: error: {written} is not allowed here; '
                  f'use {replacement} (see {document})')
            problems += 1
    if problems:
        print(f'{problems} problem(s) found. The foundation layer replaces these; '
              f'see docs/development.md, or run {Path(__file__).name} --list.')
        return 1
    print(f'Checked {len(texts)} files: each uses the foundation layer wherever it has an '
          'equivalent.')
    return 0


def list_rules():
    """Print every rule, then every authorization."""
    for group in RULES:
        by_replacement = {}
        for name, (replacement, module, document) in group.items():
            by_replacement.setdefault((module, document, replacement), []).append(name)
        for (module, document, replacement), names in by_replacement.items():
            print(f'{module} ({document}): use {replacement}')
            print(f'    in place of {" ".join(names)}')
    print()
    for authorized_path, (authorized, reason) in AUTHORIZED.items():
        print(f'{authorized_path}: {reason}')
        print(f'    may use {" ".join(sorted(authorized))}')
    return 0


def self_test():
    """Check the scanner against small sources with known answers."""
    cases = [
        ('a plain call', 'void f(void) { char *p = malloc(4); free(p); }', ['malloc', 'free']),
        ('a function used as a value', 'allocator a = { calloc, realloc };',
         ['calloc', 'realloc']),
        ('spacing before the parenthesis', 'p = strdup (text);\nq = _aligned_malloc\n(1, 2);',
         ['strdup', '_aligned_malloc']),
        ('the line of each use', '\n\nint x;\nvoid *p = alloca(8);', ['alloca']),
        ('engine functions', 'p = tb_malloc(4); tb_free(p); q = tb_try_calloc(1, 1);', []),
        ('longer identifiers', 'my_malloc(1); malloc_count++; free_list = 0; refree();', []),
        ('line comments', 'int x; // malloc(1) would be wrong here\n', []),
        ('block comments', '/* free(p);\n   calloc(1, 1); */ int y;', []),
        ('string literals', 'puts_later("call malloc() and free()");', []),
        ('escaped quotes', 'f("a \\" malloc(1) \\" b"); char c = \'\\\'\';', []),
        ('a comment marker inside a string', 'f("// not a comment"); p = malloc(1);',
         ['malloc']),
        ('a quote inside a comment', "// don't\nvoid *p = realloc(q, 2);", ['realloc']),
        ('a continued line comment', '// note \\\nmalloc(1)\nint z;', []),
        ('member access is still a use', 'table.free(p); table->malloc(1);', ['free', 'malloc']),
        ('operating system heaps', 'HeapAlloc(h, 0, 8); VirtualFree(p, 0, 0); mmap(0);',
         ['HeapAlloc', 'VirtualFree', 'mmap']),
        ('formatted output', 'printf("%d", 1); (void)snprintf(b, 4, "x"); fputs("x", stderr);',
         ['printf', 'snprintf', 'fputs']),
        ('engine formatting', 'tb_fmt_print("%d", 1); tb_fmt_buffer(b, 4, "x"); fwrite(b, 1, 1, f);',
         ['fwrite']),
        ('flushing the standard streams', 'fflush(stdout); (void)tb_fmt_flush();', ['fflush']),
        ('file calls', 'read(fd, b, n); write(fd, b, n); fopen(p, mode);',
         ['read', 'write', 'fopen']),
        ('file-like variables', 'size_t read = 0; bool write = true; ++read;', []),
        ('strings', 'n = strlen(s); if (strcmp(a, b) == 0) { p = strstr(a, b); }',
         ['strlen', 'strcmp', 'strstr']),
        ('engine strings and memory blocks',
         'n = tb_str_length(s); memcpy(a, b, n); memset(a, 0, n); x = strtoul(s, &e, 10);', []),
        ('assertions', 'assert(x > 0); _Static_assert(sizeof(int) == 4, "int"); abort();',
         ['assert', '_Static_assert', 'abort']),
        ('engine assertions', 'TB_ASSERT(x > 0); TB_STATIC_ASSERT(1, "one"); tb_platform_abort();',
         []),
        ('clocks',
         'timespec_get(&now, TIME_UTC); QueryPerformanceCounter(&counter); '
         'now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);',
         ['timespec_get', 'QueryPerformanceCounter', 'clock_gettime_nsec_np']),
        ('the engine clock', 'now = tb_platform_clock_nanoseconds();', []),
        ('native sleeps', 'Sleep(10); nanosleep(&request, NULL); usleep(5); SleepEx(1, TRUE);',
         ['Sleep', 'nanosleep', 'usleep', 'SleepEx']),
        ('the engine sleep',
         'tb_platform_sleep(10); tb_platform_wait_until(deadline); sleeper.is_asleep = true;', []),
        ('native semaphores and hints for loops that wait',
         'sem_wait(&s); ReleaseSemaphore(handle, 1, NULL); _mm_pause(); sched_yield(); '
         'key = FlsAlloc(destroy); dispatch_semaphore_signal(d);',
         ['sem_wait', 'ReleaseSemaphore', '_mm_pause', 'sched_yield', 'FlsAlloc',
          'dispatch_semaphore_signal']),
        ('the engine semaphore and hints',
         'tb_semaphore_wait(&s); tb_thread_relax(); tb_thread_yield(); '
         'tb_thread_local_set(&local, value);', []),
        ('counts of processors',
         'GetSystemInfo(&info); n = sysconf(_SC_NPROCESSORS_ONLN); sched_getaffinity(0, s, &set); '
         'n = get_nprocs();',
         ['GetSystemInfo', '_SC_NPROCESSORS_ONLN', 'sched_getaffinity', 'get_nprocs']),
        ('the engine count of processors',
         'tb_system_get_processors(&processors); n = sysconf(_SC_PAGESIZE);', []),
        ('atomic types and compiler functions',
         '_Atomic int n; __atomic_add_fetch(&n, 1, __ATOMIC_SEQ_CST); __sync_synchronize();',
         ['_Atomic', '__atomic_add_fetch', '__sync_synchronize']),
        ('every function of a family', 'InterlockedIncrement64(&n); _InterlockedOr(&n, 1);',
         ['InterlockedIncrement64', '_InterlockedOr']),
        ('names that only resemble a family',
         'tb_platform_atomic_size n; int interlocked = 0; NotInterlocked(n); tb__sync_point();',
         []),
        ('native threads and locks',
         'pthread_mutex_lock(&m); AcquireSRWLockExclusive(&l); thrd_create(&t, run, 0); '
         'cnd_wait(&c, &m); WakeAllConditionVariable(&v); pthread_t id;',
         ['pthread_mutex_lock', 'AcquireSRWLockExclusive', 'thrd_create', 'cnd_wait',
          'WakeAllConditionVariable', 'pthread_t']),
        ('other ways to a variable for each thread',
         'static __thread int a; thread_local int b; slot = TlsAlloc(); tss_set(key, p);',
         ['__thread', 'thread_local', 'TlsAlloc', 'tss_set']),
        ('engine threads and the C keyword',
         'tb_mutex_lock(&m); tb_thread_join(&t); static _Thread_local int n; '
         'thread_local_value = 1; my_pthread_create();',
         []),
        ('the thread headers', '#include <pthread.h>\n#include <threads.h>',
         ['<pthread.h>', '<threads.h>']),
        ('a replaced header', '#include <assert.h>\n# include <stdatomic.h>\n#include <malloc.h>',
         ['<assert.h>', '<stdatomic.h>', '<malloc.h>']),
        ('headers that are not replaced',
         '#include <time.h>\n#include <string.h>\n#include <stdio.h>\n#include "badger/assert.h"',
         []),
        ('a header name is not an identifier', '#include <malloc.h>', ['<malloc.h>']),
    ]
    failures = 0
    for description, source, expected in cases:
        found = [written for _, written, _, _ in find_violations('example.c', source)]
        if found != expected:
            print(f'self-test failed: {description}: expected {expected}, found {found}')
            failures += 1
    checks = len(cases)

    line = find_violations('example.c', '\n\nint x;\nvoid *p = alloca(8);')[0][0]
    checks += 1
    if line != 4:
        print(f'self-test failed: expected a use on line 4, found line {line}')
        failures += 1

    message = find_violations('example.c', 'n = strlen(s);')[0]
    checks += 1
    if message != (1, 'strlen', 'tb_str_length', 'docs/strings.md'):
        print(f'self-test failed: a violation must name its replacement and document: {message}')
        failures += 1

    authorized = find_violations('src/core/memory.c', 'p = malloc(1); q = strdup(s); free(p);')
    checks += 1
    if [written for _, written, _, _ in authorized] != ['strdup']:
        print(f'self-test failed: authorization must cover only the named functions: {authorized}')
        failures += 1

    in_directory = find_violations('src/platform/win32/example.c',
                                   'InterlockedIncrement(&n); abort(); p = malloc(1); puts("x");')
    checks += 1
    if [written for _, written, _, _ in in_directory] != ['malloc', 'puts']:
        print('self-test failed: a directory authorization must cover the files below it, and '
              f'only for its names: {in_directory}')
        failures += 1

    beside_directory = find_violations('src/platform_other.c', 'abort();')
    checks += 1
    if [written for _, written, _, _ in beside_directory] != ['abort']:
        print('self-test failed: a directory authorization must not cover a file whose name '
              f'only begins like it: {beside_directory}')
        failures += 1

    both = find_violations('tests/framework/src/support.c', 'p = calloc(1, 1); puts("x"); free(p);')
    checks += 1
    if both:
        print(f'self-test failed: a file has its own names and those of its directory: {both}')
        failures += 1

    stale = find_stale_authorizations({'src/core/memory.c': 'p = malloc(1); free(p);'})
    expected_stale = ['src/core/memory.c is authorized for calloc but does not use it',
                      'src/core/memory.c is authorized for realloc but does not use it',
                      'authorized file src/core/str.c is not in',
                      'authorized directory src/platform/ holds no sources']
    for expected in expected_stale:
        checks += 1
        if not any(message.startswith(expected) for message in stale):
            print(f'self-test failed: expected a stale authorization reported as "{expected}"')
            failures += 1
    checks += 1
    if any(message.startswith('src/core/memory.c is authorized for malloc') for message in stale):
        print('self-test failed: an authorization that is used was reported as stale')
        failures += 1

    for authorized_path, (authorized, reason) in AUTHORIZED.items():
        checks += 1
        if not authorized or not reason or not authorized <= set(FORBIDDEN):
            print(f'self-test failed: the entry for {authorized_path} needs names that rules '
                  'forbid, and a reason')
            failures += 1

    listing = '# set(NAME ignored)\nset(NAME cmake src/core)\nset(PATTERNS *.c *.h.in)\n'
    settings = [read_setting(listing, 'NAME'), read_setting(listing, 'PATTERNS'),
                read_setting(listing, 'MISSING')]
    checks += 1
    if settings != [['cmake', 'src/core'], ['*.c', '*.h.in'], None]:
        print(f'self-test failed: settings of the source list were read as {settings}')
        failures += 1

    if failures:
        return 1
    print(f'Self-test passed: {checks} checks.')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--self-test', action='store_true', help='check this script and exit')
    parser.add_argument('--list', action='store_true',
                        help='print every rule and authorization and exit')
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    if arguments.list:
        return list_rules()
    return check_project()


if __name__ == '__main__':
    sys.exit(main())
