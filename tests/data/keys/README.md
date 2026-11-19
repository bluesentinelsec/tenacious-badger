# Package keys for tests

`test.key` and `test.password` give `tests/tools/package_key_probe.c` its two
keys through `tb_add_package_key` (`cmake/BadgerPackageKey.cmake`). They are
for tests only: no real package is obfuscated with them.
