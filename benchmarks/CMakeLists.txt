# tb_bench times the public operations of the foundation modules and prints a
# line for each case. Add the benchmarks of a new module to the sources here,
# and its entry point to harness/tb_bench.h and harness/tb_bench.c.
add_executable(tb_bench
    main.c
    harness/tb_bench.c
    codec/base64_bench.c
    codec/chacha20_bench.c
    codec/checksum_bench.c
    codec/deflate_bench.c
    codec/sha256_bench.c
    codec/tar_bench.c
    codec/zip_bench.c
    codec/package_bench.c
    codec/inflate_bench.c
    core/ap_bench.c
    core/array_bench.c
    core/atom_bench.c
    core/bit_bench.c
    core/decimal_bench.c
    core/diagnostics_bench.c
    core/fmt_bench.c
    core/fs_bench.c
    core/hash_bench.c
    core/memory_bench.c
    core/pool_bench.c
    core/ring_bench.c
    core/rng_bench.c
    core/sort_bench.c
    core/stream_bench.c
    core/table_bench.c
    core/text_bench.c
    core/text_units_bench.c
    core/xp_bench.c
    jobs/jobs_bench.c
    serialize/json_bench.c
    vfs/vfs_bench.c
    platform/atomic_bench.c
    platform/thread_bench.c
    platform/time_bench.c)
target_include_directories(tb_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/harness"
    # The streams that the decompression tests decode are what it times.
    "${PROJECT_SOURCE_DIR}/tests/codec")
# The shared library, as burrow uses it: the calls that are timed cross the
# same boundary as the game's.
target_link_libraries(tb_bench PRIVATE TenaciousBadger::badger)
tb_configure_target(tb_bench)

# Timings are not tested: no machine that runs the tests is quiet enough. What
# is tested is that every benchmark still runs, at a small size, so that a
# change to a module cannot leave its benchmark broken.
if(TB_BUILD_TESTS)
    add_test(NAME benchmarks.every_case_runs COMMAND tb_bench --quick)
    add_test(NAME benchmarks.an_unknown_module_is_a_usage_error COMMAND tb_bench no_such_module)
    set_tests_properties(benchmarks.an_unknown_module_is_a_usage_error PROPERTIES
        WILL_FAIL TRUE)
endif()
