# Conventions of the public API

What every module of the engine does the same way, so that a caller who has
learned one module has learned the shape of the next. [AGENTS.md](../AGENTS.md)
has the rules for writing the code; this page has the rules for the interface
that a caller sees. A new module follows them, and a change that departs from
one says why in its pull request.

## How a function reports failure

There are two kinds of failure, and they are never reported the same way.

**A caller that broke the contract** passed something the documentation
forbids: a null pointer where one is required, an index past the end, a call
from the wrong thread. That is a bug in the caller. It fails an assertion in a
build with assertions, and nothing is returned for it.

**A failure at run time** is something a correct program can meet: a file
that is not there, a disk that is full, a buffer that is too small for the
answer. The function returns it, and the caller decides what to do.

A module whose functions can fail at run time in more than one way has one
enumeration for all of them:

| Rule | Example |
| --- | --- |
| The type is `tb_<module>_result` | `tb_fs_result`, `tb_path_result` |
| Success is the value zero, named `TB_<MODULE>_OK` | `TB_FS_OK` |
| A fallible function returns the result, and gives what it produces through pointers | `tb_fs_result tb_file_open(path, flags, &file)` |
| What an output holds after a failure is documented for each function | "unchanged on error" |
| `tb_<module>_result_name(result)` names a result for a log or a message | `tb_fs_result_name(TB_FS_NOT_FOUND)` is `"not found"` |

The name is a short phrase in lower case, with static storage duration. A
value that is no result of the module is named `"unknown result"`.

**Each module has its own enumeration.** One enumeration for the whole engine
would let every function return anything, and a caller could no longer tell
from a function's type what it has to handle. What the modules share is the
names of the conditions that more than one of them meets:

| Name | Meaning | Named |
| --- | --- | --- |
| `_OK` | Success | `"ok"` |
| `_INVALID` | An argument or an input is malformed: bad UTF-8, an unknown flag, a size that is not a multiple of a page | `"invalid argument"` |
| `_NO_MEMORY` | Memory, address space, or a native resource such as a handle ran out | `"out of memory"` |
| `_TOO_LARGE` | A size or a length is over a limit, or cannot be represented | `"too large"` |
| `_BUFFER_TOO_SMALL` | The caller's buffer cannot hold the result. The function says how large it must be, and writes nothing | `"buffer too small"` |
| `_UNAVAILABLE` | An optional capability is missing on this machine or for this process. The caller falls back; this is never fatal | `"unavailable"` |
| `_OS_ERROR` | The operating system reported a failure that no other result names | `"operating system error"` |

A module names a condition of its own as it likes: `TB_FS_NOT_EMPTY`,
`TB_PATH_ESCAPE`, `TB_LOG_FILTERED`. It does not give a shared condition a
name of its own, and does not use a shared name for something else.

**Running out of memory is the exception that proves the rule.** The
allocation functions end the process when memory runs out, and have `tb_try_`
forms for callers that can recover; see [memory](memory.md). A module returns
`_NO_MEMORY` only where a caller can reasonably carry on without the memory,
as when a file is too large to read.

A function with exactly one way to fail may return `bool`, or a pointer that
is `NULL`, and need no enumeration: `tb_utf8_validate`, `tb_atom_find`.

## Headers and prefixes

- **One public header for a module**, named for the module:
  `badger/table.h` declares `tb_table_*`. Including it is enough to use the
  module.
- **A platform interface has a header of its own**, because every function in
  it is implemented once for each operating system and the build checks that
  list: `platform.h`, `thread.h`, `system.h`, `virtual_memory.h`, `file.h`.
- **Where the engine builds on a platform interface, the module's header
  includes the interface's.** `badger/fs.h` is the header of the file system
  module. It includes `badger/file.h`, which holds the part that each
  operating system implements: file and directory handles, and the operations
  on names. Both use the `tb_fs_` prefix for what is about the file system as
  a whole, and code that includes `fs.h` has all of it.
- **A function that needs nothing from the platform is not part of an
  adapter**, even when a platform interface's header declares it:
  `tb_vm_result_name` is implemented once, by the engine.

## Threading

Every public function documents one of three contracts, in these words:

- *thread-safe*: internally synchronized, or immutable;
- *stage-owned*: called only from a documented stage or thread;
- *externally synchronized*: the caller serializes access.

A function states its contract under `@par Threading` in its own comment. A
header whose functions nearly all have the same contract states it once,
under `@par Threading` in the comment that opens the file, and a function
with a different contract says so in its own. `tools/check_contracts.py`
fails, in CI and as the `source_checks.contracts` test, on a public function
that neither statement covers, or whose statement uses none of the three
terms.

The check reads the words; it cannot tell whether they are true. That is what
tests under [ThreadSanitizer](testing.md#data-races) are for. A module that
synchronizes itself has cases that use one object from several threads.
Functions that keep no state, and objects that are documented as independent
of one another, are used from eight threads at once in
`tests/core/thread_safety_test.c`, each thread with data of its own.

**Externally synchronized objects check it.** In builds with assertions, an
arena, a dynamic array, a hash table, a set, a ring buffer, and a bit set
each record which thread is inside a function that changes it. A second
thread that calls such a function while the first is still inside fails an
assertion that names the kind of object, on every platform, Windows included,
where there is no ThreadSanitizer. The guard lives in `src/core/exclusive.h`.

- **What is checked:** functions that change the object, its destruction
  included. Functions that only read take no part, so readers still run
  together, and a reader that overlaps a writer is not caught.
- **Re-entry:** a thread may enter an object it is already inside. That also
  covers a test that leaves a function through a trap: the object stays with
  the test's own thread, which can go on using it and destroy it.
- **Cost:** a compare-exchange and a store for each call, about 17 ns in a
  Debug build (Release, without assertions, has neither).
- **Not covered:** the intrusive list (`badger/list.h`), whose structure the
  caller declares, so it has no room for a guard; and stage-owned functions,
  which are for single-threaded startup.

## Text files

Source and documentation are UTF-8 without a byte-order mark.
`tools/check_text.py` checks every tracked file, in CI and as the
`source_checks.text` test.
