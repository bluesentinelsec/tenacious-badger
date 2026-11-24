/* DXC on Windows: dxcompiler.dll and dxil.dll, loaded at run time from
 * beside the executable or from the Windows SDK, and called through COM's C
 * binding (see dxc_api.h). */
#include "badger/dxc.h"

#include "badger/fmt.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/utf8.h"
#include "dxc_api.h"

#include <stdarg.h>
#include <string.h>

/* The SDK's directory of libraries for the processor the engine runs on. */
#if defined(_M_ARM64)
#define TB_DXC_ARCHITECTURE "arm64"
#elif defined(_M_IX86)
#define TB_DXC_ARCHITECTURE "x86"
#else
#define TB_DXC_ARCHITECTURE "x64"
#endif

/* Where the registry names the Windows SDK's root. */
#define KITS_KEY L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots"
#define KITS_VALUE L"KitsRoot10"

/* RegGetValueW's flags, as winreg.h has them: a string, read in the
 * registry's own view, then in that of 32-bit programs, where the SDK's
 * installer writes. */
#define REG_STRING_FLAG 0x00000002u
#define REG_32_BIT_VIEW_FLAG 0x00020000u

/* UTF-16 units of the SDK's root as the registry holds it. */
#define ROOT_CAPACITY ((size_t)1024)

/* Bytes of a path built from the SDK's root. */
#define PATH_CAPACITY ((size_t)4096)

/* Arguments a compile passes DXC at most: the name, -T, -E, -HV, and the
 * debug or optimization flags. */
#define MAX_ARGUMENTS ((size_t)12)

/* Strings a compile converts to UTF-16: the name, the profile and the entry point. */
#define WIDE_STRING_COUNT ((size_t)3)

struct tb_dxc
{
    tb_library *signer;   /* dxil.dll */
    tb_library *compiler; /* dxcompiler.dll */
    tb_dxc_create_instance_function create;
    tb_dxc_create_instance_function create_validator; /* dxil.dll's */
    char version[TB_DXC_VERSION_CAPACITY];
};

struct tb_dxc_compiled
{
    tb_dxc_result_object *result;
    tb_dxc_blob *code;
    tb_dxc_blob *messages;
};

typedef void(WINAPI *co_task_mem_free_function)(LPVOID memory);

typedef LONG(WINAPI *reg_get_value_function)(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags,
                                             LPDWORD type, PVOID data, LPDWORD size);

static tb_dxc_result fail(tb_dxc_error *error, tb_dxc_result result, const char *format, ...)
{
    if (error != NULL)
    {
        va_list arguments;
        va_start(arguments, format);
        error->result = result;
        (void)tb_fmt_vbuffer(error->message, sizeof(error->message), format, arguments);
        va_end(arguments);
    }
    return result;
}

static void succeed(tb_dxc_error *error)
{
    if (error != NULL)
    {
        error->result = TB_DXC_OK;
        error->message[0] = '\0';
    }
}

/* -------------------------------------------------------------------------
 * Finding DXC.
 * ------------------------------------------------------------------------- */

/* The Windows SDK's root as the registry names it, in UTF-8, with a final
 * `\`; false, with `why` saying why, when there is none. */
static bool read_kits_root(char *root, size_t capacity, char *why, size_t why_capacity)
{
    static const DWORD views[] = {REG_STRING_FLAG, REG_STRING_FLAG | REG_32_BIT_VIEW_FLAG};
    tb_library *advapi = NULL;
    tb_library_function function = NULL;
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    reg_get_value_function get_value = NULL;
    WCHAR wide[ROOT_CAPACITY] = {0};
    size_t units = 0;
    size_t written = 0;
    bool found = false;
    if (tb_library_open("advapi32.dll", TB_LIBRARY_SEARCH_SYSTEM, &advapi, &error) !=
            TB_LIBRARY_OK ||
        tb_library_find_function(advapi, "RegGetValueW", &function, &error) != TB_LIBRARY_OK)
    {
        /* coverage-off: advapi32.dll and its RegGetValueW are part of every Windows */
        (void)tb_fmt_buffer(why, why_capacity, "%s", error.message);
        goto cleanup;
        /* coverage-on */
    }
    get_value = (reg_get_value_function)function;
    for (size_t view = 0; view < sizeof(views) / sizeof(views[0]) && !found; view++)
    {
        DWORD size = (DWORD)(sizeof(wide) - sizeof(WCHAR));
        found = get_value(HKEY_LOCAL_MACHINE, KITS_KEY, KITS_VALUE, views[view], NULL, wide,
                          &size) == ERROR_SUCCESS;
    }
    if (!found)
    {
        /* coverage-off: the machines the tests run on have the Windows SDK */
        (void)tb_fmt_buffer(why, why_capacity, "the Windows SDK is not installed");
        goto cleanup;
        /* coverage-on */
    }
    units = wcslen(wide);
    found = units > 0 && wide[units - 1] == L'\\' &&
            tb_utf8_from_utf16((const uint16_t *)wide, units, root, capacity - 1, &written);
    if (!found)
    {
        /* coverage-off: the SDK's installer writes a directory, with its final separator */
        (void)tb_fmt_buffer(why, why_capacity, "the registry's KitsRoot10 is not a directory");
        goto cleanup;
        /* coverage-on */
    }
    root[written] = '\0';
cleanup:
    tb_library_close(advapi);
    return found;
}

/* Load dxil.dll, then dxcompiler.dll, from one place: the executable's
 * directory when `directory` is NULL, else `directory`, which ends in `\`.
 * dxil.dll comes first, so that dxcompiler.dll, which loads it by name to
 * sign what it compiles, finds that one already loaded. */
static bool load_from(tb_dxc *dxc, const char *directory, tb_library_error *error)
{
    const tb_library_search search =
        directory == NULL ? TB_LIBRARY_SEARCH_APPLICATION : TB_LIBRARY_SEARCH_PATH;
    const char *const prefix = directory == NULL ? "" : directory;
    char signer[PATH_CAPACITY] = {0};
    char compiler[PATH_CAPACITY] = {0};
    tb_library_function create = NULL;
    tb_library_function create_validator = NULL;
    (void)tb_fmt_buffer(signer, sizeof(signer), "%sdxil.dll", prefix);
    (void)tb_fmt_buffer(compiler, sizeof(compiler), "%sdxcompiler.dll", prefix);
    if (tb_library_open(signer, search, &dxc->signer, error) != TB_LIBRARY_OK ||
        tb_library_open(compiler, search, &dxc->compiler, error) != TB_LIBRARY_OK ||
        tb_library_find_function(dxc->compiler, "DxcCreateInstance", &create, error) !=
            TB_LIBRARY_OK ||
        tb_library_find_function(dxc->signer, "DxcCreateInstance", &create_validator, error) !=
            TB_LIBRARY_OK)
    {
        tb_library_close(dxc->compiler);
        tb_library_close(dxc->signer);
        dxc->compiler = NULL;
        dxc->signer = NULL;
        return false;
    }
    dxc->create = (tb_dxc_create_instance_function)create;
    dxc->create_validator = (tb_dxc_create_instance_function)create_validator;
    return true;
}

/* Load DXC from the Windows SDK's redistributable directory; false, with
 * `why` saying why, when it is not there. */
static bool load_from_sdk(tb_dxc *dxc, char *why, size_t why_capacity)
{
    char root[PATH_CAPACITY] = {0};
    char directory[PATH_CAPACITY] = {0};
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    if (!read_kits_root(root, sizeof(root), why, why_capacity))
    {
        /* coverage-off: the machines the tests run on have the Windows SDK */
        return false;
        /* coverage-on */
    }
    (void)tb_fmt_buffer(directory, sizeof(directory), "%sRedist\\D3D\\" TB_DXC_ARCHITECTURE "\\",
                        root);
    if (!load_from(dxc, directory, &error))
    {
        /* coverage-off: the machines the tests run on have the SDK's DXC */
        (void)tb_fmt_buffer(why, why_capacity, "%s", error.message);
        return false;
        /* coverage-on */
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Naming DXC.
 * ------------------------------------------------------------------------- */

/* Write one part of DXC, `name`, which `create` makes as `clsid`, as
 * IDxcVersionInfo2 tells it: `dxil 1.8 (4775, 1b4f3c2de)`; its version alone
 * where it tells no commit, and `unknown` where it tells nothing. Returns the
 * bytes written. */
static size_t describe_part(tb_dxc_create_instance_function create, const GUID *clsid,
                            const char *name, co_task_mem_free_function free_memory, char *buffer,
                            size_t capacity)
{
    tb_dxc_version_info *info = NULL;
    tb_dxc_version_info *info2 = NULL;
    UINT32 major = 0;
    UINT32 minor = 0;
    UINT32 commits = 0;
    char *hash = NULL;
    size_t written = 0;
    if (FAILED(create(clsid, &tb_dxc_iid_version_info, (void **)&info)) ||
        FAILED(info->lpVtbl->GetVersion(info, &major, &minor)))
    {
        /* coverage-off: every DXC since 1.0 tells its version */
        written = tb_fmt_buffer(buffer, capacity, "%s unknown", name);
        goto cleanup;
        /* coverage-on */
    }
    if (free_memory != NULL &&
        SUCCEEDED(info->lpVtbl->QueryInterface(info, &tb_dxc_iid_version_info2, (void **)&info2)) &&
        SUCCEEDED(info2->lpVtbl->GetCommitInfo(info2, &commits, &hash)) && hash != NULL)
    {
        written = tb_fmt_buffer(buffer, capacity, "%s %u.%u (%u, %s)", name, (unsigned)major,
                                (unsigned)minor, (unsigned)commits, hash);
        free_memory(hash);
        goto cleanup;
    }
    /* The Windows SDK's validator tells no commit. */
    written = tb_fmt_buffer(buffer, capacity, "%s %u.%u", name, (unsigned)major, (unsigned)minor);
cleanup:
    if (info2 != NULL)
    {
        (void)info2->lpVtbl->Release(info2);
    }
    if (info != NULL)
    {
        (void)info->lpVtbl->Release(info);
    }
    return written < capacity ? written : capacity - 1;
}

/* Name the compiler and the validator loaded, into `dxc->version`. The
 * commit's hash is DXC's to give and the caller's to free, with ole32.dll's
 * CoTaskMemFree(); without it, the hash is left out. */
static void describe(tb_dxc *dxc)
{
    tb_library *ole32 = NULL;
    tb_library_function function = NULL;
    co_task_mem_free_function free_memory = NULL;
    if (tb_library_open("ole32.dll", TB_LIBRARY_SEARCH_SYSTEM, &ole32, NULL) == TB_LIBRARY_OK &&
        tb_library_find_function(ole32, "CoTaskMemFree", &function, NULL) == TB_LIBRARY_OK)
    {
        free_memory = (co_task_mem_free_function)function;
    }
    const size_t written = describe_part(dxc->create, &tb_dxc_clsid_compiler, "dxcompiler",
                                         free_memory, dxc->version, sizeof(dxc->version));
    (void)tb_fmt_buffer(dxc->version + written, sizeof(dxc->version) - written, " ");
    if (written + 1 < sizeof(dxc->version))
    {
        (void)describe_part(dxc->create_validator, &tb_dxc_clsid_validator, "dxil", free_memory,
                            dxc->version + written + 1, sizeof(dxc->version) - written - 1);
    }
    tb_library_close(ole32);
}

const char *tb_dxc_version(const tb_dxc *dxc)
{
    return dxc->version;
}

tb_dxc_result tb_dxc_open(tb_dxc **dxc, tb_dxc_error *error)
{
    tb_dxc *const loaded = tb_calloc(1, sizeof(*loaded));
    tb_library_error beside = TB_LIBRARY_ERROR_INIT;
    char why[TB_DXC_MESSAGE_CAPACITY] = {0};
    *dxc = NULL;
    if (!load_from(loaded, NULL, &beside) && !load_from_sdk(loaded, why, sizeof(why)))
    {
        /* coverage-off: the machines the tests run on have the SDK's DXC */
        tb_free(loaded);
        return fail(error, TB_DXC_UNAVAILABLE,
                    "DXC (dxcompiler.dll and dxil.dll) is neither beside the executable nor in "
                    "the Windows SDK (%s); install the Windows SDK, or copy both beside the "
                    "executable",
                    why);
        /* coverage-on */
    }
    describe(loaded);
    *dxc = loaded;
    succeed(error);
    return TB_DXC_OK;
}

void tb_dxc_close(tb_dxc *dxc)
{
    if (dxc == NULL)
    {
        return;
    }
    tb_library_close(dxc->compiler);
    tb_library_close(dxc->signer);
    tb_free(dxc);
}

/* -------------------------------------------------------------------------
 * Compiling.
 * ------------------------------------------------------------------------- */

/* A UTF-16 copy of a UTF-8 string, from tb_calloc; NULL when the string is
 * not valid UTF-8. */
static WCHAR *widen(const char *text)
{
    const size_t length = tb_str_length(text);
    size_t units = 0;
    if (!tb_utf8_to_utf16(text, length, NULL, 0, &units))
    {
        return NULL;
    }
    WCHAR *const wide = tb_calloc(units + 1, sizeof(WCHAR));
    (void)tb_utf8_to_utf16(text, length, (uint16_t *)wide, units, &units);
    return wide;
}

static bool is_empty(const char *text)
{
    return text == NULL || text[0] == '\0';
}

static tb_dxc_result check_request(const tb_dxc_request *request, tb_dxc_error *error)
{
    if (is_empty(request->name) || is_empty(request->profile))
    {
        return fail(error, TB_DXC_INVALID, "a DXC compile names its source and its profile");
    }
    if (request->name[0] == '-' || request->name[0] == '/')
    {
        return fail(error, TB_DXC_INVALID, "a DXC compile's name '%s' would be read as an option",
                    request->name);
    }
    if (is_empty(request->entry_point) && !tb_str_starts_with(request->profile, "lib_"))
    {
        return fail(error, TB_DXC_INVALID, "a DXC compile for %s names its entry point",
                    request->profile);
    }
    if (request->source == NULL && request->source_length > 0)
    {
        return fail(error, TB_DXC_INVALID, "a DXC compile of %zu bytes has no source",
                    request->source_length);
    }
    return TB_DXC_OK;
}

/* What a failed HRESULT of DXC's own is, as a result. */
static tb_dxc_result failure_of(HRESULT status)
{
    return status == E_OUTOFMEMORY ? TB_DXC_NO_MEMORY : TB_DXC_FAILED;
}

/* Where the line that starts at `start` ends: at its line break, or at `length`. */
static size_t line_end(const char *text, size_t start, size_t length)
{
    const size_t end = tb_str_find_char(text, start, length, '\n');
    return end == TB_STR_NOT_FOUND ? length : end;
}

/* Find the first line of DXC's messages that holds `pattern`: false when
 * none does, which leaves `begin` and `end` as they were. */
static bool find_line(const char *messages, size_t length, const char *pattern, size_t *begin,
                      size_t *end)
{
    for (size_t start = 0; start < length;)
    {
        const size_t stop = line_end(messages, start, length);
        if (tb_str_find(messages, start, stop, pattern) != TB_STR_NOT_FOUND)
        {
            *begin = start;
            *end = stop;
            return true;
        }
        start = stop + 1;
    }
    return false;
}

/* Say what failed with DXC's first error that names a place; else its first
 * error, which may be the summary its validator writes before the errors
 * themselves; else its first line. */
static void report_first_error(const char *messages, size_t length, tb_dxc_error *error)
{
    size_t begin = 0;
    size_t end = line_end(messages, 0, length);
    if (!find_line(messages, length, ": error: ", &begin, &end))
    {
        (void)find_line(messages, length, "error: ", &begin, &end);
    }
    (void)fail(error, TB_DXC_FAILED, "%.*s", (int)(end - begin), messages + begin);
}

/* The arguments of a compile, in `arguments`, from the request and from
 * `strings`, its name, profile and entry point in UTF-16. Returns their count. */
static UINT32 build_arguments(const tb_dxc_request *request, WCHAR *const *strings,
                              LPCWSTR *arguments)
{
    UINT32 count = 0;
    arguments[count++] = strings[0];
    arguments[count++] = L"-T";
    arguments[count++] = strings[1];
    if (strings[2] != NULL)
    {
        arguments[count++] = L"-E";
        arguments[count++] = strings[2];
    }
    arguments[count++] = L"-HV";
    arguments[count++] = L"2021";
    if ((request->flags & TB_DXC_DEBUG) != 0)
    {
        arguments[count++] = L"-Od";
        arguments[count++] = L"-Zi";
        arguments[count++] = L"-Qembed_debug";
    }
    else
    {
        arguments[count++] = L"-O3";
    }
    return count;
}

/* Read what a compile gave into `output`. */
static tb_dxc_result read_result(tb_dxc_compiled *compiled, tb_dxc_output *output,
                                 tb_dxc_error *error)
{
    tb_dxc_result_object *const object = compiled->result;
    HRESULT status = S_OK;
    (void)object->lpVtbl->GetStatus(object, &status);
    (void)object->lpVtbl->GetOutput(object, TB_DXC_OUT_ERRORS, &tb_dxc_iid_blob_utf8,
                                    (void **)&compiled->messages, NULL);
    output->messages = "";
    if (compiled->messages != NULL)
    {
        output->messages = compiled->messages->lpVtbl->GetStringPointer(compiled->messages);
        output->messages_length = compiled->messages->lpVtbl->GetStringLength(compiled->messages);
    }
    if (FAILED(status))
    {
        report_first_error(output->messages, output->messages_length, error);
        return TB_DXC_FAILED;
    }
    (void)object->lpVtbl->GetOutput(object, TB_DXC_OUT_OBJECT, &tb_dxc_iid_blob,
                                    (void **)&compiled->code, NULL);
    if (compiled->code == NULL)
    {
        /* coverage-off: a compile that succeeds has its object */
        return fail(error, TB_DXC_FAILED, "DXC succeeded, but gave no DXIL");
        /* coverage-on */
    }
    output->code = compiled->code->lpVtbl->GetBufferPointer(compiled->code);
    output->code_size = compiled->code->lpVtbl->GetBufferSize(compiled->code);
    succeed(error);
    return TB_DXC_OK;
}

tb_dxc_result tb_dxc_compile(tb_dxc *dxc, const tb_dxc_request *request, tb_dxc_output *output,
                             tb_dxc_error *error)
{
    const tb_dxc_output empty = {0};
    WCHAR *strings[WIDE_STRING_COUNT] = {0};
    LPCWSTR arguments[MAX_ARGUMENTS] = {0};
    UINT32 count = 0;
    tb_dxc_compiler *compiler = NULL;
    tb_dxc_compiled *compiled = NULL;
    tb_dxc_buffer source = {0};
    HRESULT status = S_OK;
    tb_dxc_result result = check_request(request, error);
    *output = empty;
    if (result != TB_DXC_OK)
    {
        return result;
    }
    strings[0] = widen(request->name);
    strings[1] = widen(request->profile);
    strings[2] = is_empty(request->entry_point) ? NULL : widen(request->entry_point);
    if (strings[0] == NULL || strings[1] == NULL ||
        (strings[2] == NULL && !is_empty(request->entry_point)))
    {
        result = fail(error, TB_DXC_INVALID, "a DXC compile's names are not UTF-8");
        goto cleanup;
    }
    count = build_arguments(request, strings, arguments);
    status = dxc->create(&tb_dxc_clsid_compiler, &tb_dxc_iid_compiler3, (void **)&compiler);
    if (FAILED(status))
    {
        /* coverage-off: DXC makes a compiler unless memory runs out */
        result = fail(error, failure_of(status), "DXC made no compiler (HRESULT 0x%08lx)",
                      (unsigned long)status);
        goto cleanup;
        /* coverage-on */
    }
    compiled = tb_calloc(1, sizeof(*compiled));
    source.pointer = request->source;
    source.size = request->source_length;
    source.encoding = TB_DXC_CP_UTF8;
    status = compiler->lpVtbl->Compile(compiler, &source, arguments, count, NULL,
                                       &tb_dxc_iid_result, (void **)&compiled->result);
    if (FAILED(status))
    {
        /* coverage-off: DXC reports a source's errors in its result, not here */
        result = fail(error, failure_of(status), "DXC failed to compile (HRESULT 0x%08lx)",
                      (unsigned long)status);
        goto cleanup;
        /* coverage-on */
    }
    result = read_result(compiled, output, error);
    output->compiled = compiled;
    compiled = NULL;
cleanup:
    if (compiled != NULL)
    {
        /* coverage-off: only a compile DXC could not run gets here with a result to drop */
        tb_dxc_output dropped = {0};
        dropped.compiled = compiled;
        tb_dxc_release(&dropped);
        /* coverage-on */
    }
    if (compiler != NULL)
    {
        (void)compiler->lpVtbl->Release(compiler);
    }
    for (size_t index = 0; index < WIDE_STRING_COUNT; index++)
    {
        tb_free(strings[index]);
    }
    return result;
}

void tb_dxc_release(tb_dxc_output *output)
{
    tb_dxc_compiled *const compiled = output->compiled;
    const tb_dxc_output empty = {0};
    *output = empty;
    if (compiled == NULL)
    {
        return;
    }
    if (compiled->code != NULL)
    {
        (void)compiled->code->lpVtbl->Release(compiled->code);
    }
    if (compiled->messages != NULL)
    {
        (void)compiled->messages->lpVtbl->Release(compiled->messages);
    }
    if (compiled->result != NULL)
    {
        (void)compiled->result->lpVtbl->Release(compiled->result);
    }
    tb_free(compiled);
}
