# Logging

`badger/log.h` provides synchronous logging with TRACE through FATAL severity,
an optional exact category filter, and replaceable record/flush callbacks.
FATAL is a severity label; the caller decides whether to terminate. The default
logger accepts INFO and above and writes `[LEVEL] [category] message` to stderr.
There is no background thread or queue. A native file sink borrows an open file
handle and writes at its cursor; the caller chooses creation/append positioning
and closes the handle after the logger releases it.

One logger serializes whole records and configuration changes. Callback records
and messages are borrowed only for that call. Configuration borrows its category
filter and context until a successful replacement or destruction returns; the
caller can then release the old state. Replacement does not flush the old sink.
Callbacks must return normally and must not wait for another thread that needs
a logger. Recursive calls into any logger return REENTRANT before taking a lock.
Different loggers sharing a sink require external synchronization. Destruction
requires exclusive lifetime access and flushes once, consuming the logger even
if flushing fails. Logging is not safe in a signal handler.

Messages are bounded UTF-8 without embedded NUL. The raw write API accepts any
length; the printf API uses a 4096-byte stack buffer and reports TOO_LARGE rather
than truncating. Rejected messages are not formatted. Formatting uses badger's
formatter and allocates no memory. Write/flush failures return IO, possibly after
a partial record, and never recursively log an error. A custom sink can recover
on a subsequent call. The stderr sink flushes each record and reports stderr's
sticky error state independently of stdout; file writes require explicit flush
or logger destruction for a native flush. No ordering or atomicity is promised
against writers outside the logger.

## Writing the log to a file

The default logger, `tb_fmt_print_error()` and the crash reporter all write
to standard error, and the programs' results go to standard output. Where
nothing reads those streams they are lost: a game started from a launcher
or a desktop shortcut, and every Windows program run under Proton, whose
streams go nowhere a player or a developer can see. Burrow and the sandbox
take `--log-file FILE` for that:

```
burrow --log-file burrow.log
proton run badger-sandbox.exe --log-file sandbox.log --frames 120
```

The file is made, or emptied, before the program writes anything, and both
streams go to it from then on, unbuffered, in the order they are written;
nothing reaches the console. A file that cannot be made, such as one in a
directory that does not exist, is an error that ends the program before it
starts, said on standard error. The file is open for others to read while
the program runs, so `tail -f` follows it.

A program of its own does the same: read its command line, call
`tb_process_write_output_to()` ([platforms](platforms.md#output-to-a-file))
with the file, then start. Moving the streams rather than adding a sink
catches what the logger never sees: a C runtime's assertion, and a crash's
stack.

Unreal writes its log to `Saved/Logs/<Game>.log` whether asked or not, and
`-log` opens a console for it as well. The engine writes no file unless asked:
the file is the developer's choice, named on the command line.

## Testing

Tests cover all severity thresholds, category filters, invalid UTF-8, format
bounds, recursive callbacks, failure/recovery, native file ownership and exact
output, concurrent writers with configuration replacement, and stderr failures.
The logging source runs under ASan/UBSan/TSan where supported, alongside Debug
and Release tests on all three platforms, and is held to the
[coverage](testing.md#coverage) that every source of the engine is.
