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

Tests cover all severity thresholds, category filters, invalid UTF-8, format
bounds, recursive callbacks, failure/recovery, native file ownership and exact
output, concurrent writers with configuration replacement, and stderr failures.
The logging source runs under ASan/UBSan/TSan where supported, alongside Debug
and Release tests on all three platforms, and is held to the
[coverage](testing.md#coverage) that every source of the engine is.
