# Installed foundation consumer

This standalone C17 utility builds with `find_package(TenaciousBadger)` and only
public headers. Both executables copy a binary file atomically, using the same
source against the shared or static engine library. File contents live in a
reserved arena. Diagnostics go through the synchronous logger.

```
foundation_shared request.txt [maximum-bytes] [reserve-bytes]
```

The UTF-8 request contains the source path on its first line and the destination
on its second; a final newline is optional and LF/CRLF are accepted. Paths cannot
contain newline, carriage return, or NUL. Parents must already exist. The request
filename passed through argv must be ASCII so this example does not assume a
Windows narrow-command-line encoding. The paths inside the request support
Unicode on every platform. They retain native symlink/parent resolution.

The default payload limit is 16 MiB and the default reservation is 64 MiB. Try a
small reservation to exercise allocation failure. Errors leave the old output
unchanged until replacement succeeds. An output/logging error after successful
replacement can still yield a failure status; this is not a rollback guarantee.
Atomic visibility does not imply power-loss durability.

Configure with `-DCMAKE_PREFIX_PATH=/path/to/sdk`, build, then run CTest. The
optional `TB_CONSUMER_SIGNING_MODULE` supplies development signing policy on
Windows; CI copies that policy separately and signs both executables. No engine
implementation, private header, or dependency download enters the consumer.

`tools/verify_foundation.py` copies this directory outside the checkout and builds
and tests it against the installed SDK. CI runs both variants in Debug and
Release on every desktop platform, then repeats verification against the
extracted SDK archive. Tests cover Unicode names, binary sizes through 1 MiB,
invalid UTF-8/requests, missing inputs/parents, size limits, reservation failure,
old-file preservation, and temporary-file cleanup.
