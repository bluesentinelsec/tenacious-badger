/* Calls each function of the default platform adapter, the one a platform
 * without an adapter of its own is built with. Every function must report
 * "not implemented" and end the process, so each case here is expected to
 * end the process: ExpectPlatformDefault.cmake runs each as a child process and
 * checks how it ends. They are not registered with CTest directly.
 *
 * A case is named after the function it calls. The script reads the public
 * headers for the functions, so a function that is added to a platform
 * interface without a case here fails the test. */
#include "badger/crash.h"
#include "badger/library.h"
#include "badger/platform.h"
#include "badger/process.h"
#include "badger/random.h"
#include "badger/stack.h"
#include "badger/system.h"
#include "badger/thread.h"
#include "badger/window.h"

#include "tb_test.h"

static void do_nothing(void *argument)
{
    (void)argument;
}

TB_TEST(platform_default, tb_platform_abort)
{
    tb_platform_abort();
}

TB_TEST(platform_default, tb_platform_write_error)
{
    tb_platform_write_error("x", 1);
}

TB_TEST(platform_default, tb_crash_catch)
{
    tb_crash_catch();
}

TB_TEST(platform_default, tb_crash_report_abort)
{
    tb_crash_report_abort(0);
}

TB_TEST(platform_default, tb_crash_write_dumps)
{
    (void)tb_crash_write_dumps(NULL);
}

TB_TEST(platform_default, tb_library_open)
{
    tb_library *library = NULL;

    (void)tb_library_open("library", TB_LIBRARY_SEARCH_SYSTEM, &library, NULL);
}

TB_TEST(platform_default, tb_library_close)
{
    tb_library_close(NULL);
}

TB_TEST(platform_default, tb_library_find_function)
{
    tb_library_function function = NULL;

    (void)tb_library_find_function(NULL, "function", &function, NULL);
}

TB_TEST(platform_default, tb_library_find_data)
{
    void *data = NULL;

    (void)tb_library_find_data(NULL, "data", &data, NULL);
}

TB_TEST(platform_default, tb_library_bind)
{
    (void)tb_library_bind(NULL, NULL, 0, NULL);
}

TB_TEST(platform_default, tb_process_get_arguments)
{
    char program[] = "program";
    char *values[] = {program, NULL};
    tb_arguments arguments = TB_ARGUMENTS_INIT;

    (void)tb_process_get_arguments(1, values, &arguments);
}

TB_TEST(platform_default, tb_process_release_arguments)
{
    tb_arguments arguments = TB_ARGUMENTS_INIT;

    tb_process_release_arguments(&arguments);
}

TB_TEST(platform_default, tb_platform_clock_nanoseconds)
{
    (void)tb_platform_clock_nanoseconds();
}

TB_TEST(platform_default, tb_stack_capture)
{
    void *frames[1] = {0};

    (void)tb_stack_capture(frames, 1, 0);
}

TB_TEST(platform_default, tb_stack_describe)
{
    tb_stack_location location = {0};

    tb_stack_describe(NULL, &location);
}

TB_TEST(platform_default, tb_platform_sleep)
{
    tb_platform_sleep(1);
}

TB_TEST(platform_default, tb_platform_wait_until)
{
    tb_platform_wait_until(1);
}

TB_TEST(platform_default, tb_platform_atomic_size_add)
{
    tb_platform_atomic_size atomic = {0};

    (void)tb_platform_atomic_size_add(&atomic, 1);
}

TB_TEST(platform_default, tb_platform_atomic_size_subtract)
{
    tb_platform_atomic_size atomic = {0};

    (void)tb_platform_atomic_size_subtract(&atomic, 1);
}

TB_TEST(platform_default, tb_platform_atomic_size_load)
{
    tb_platform_atomic_size atomic = {0};

    (void)tb_platform_atomic_size_load(&atomic);
}

TB_TEST(platform_default, tb_platform_atomic_size_store)
{
    tb_platform_atomic_size atomic = {0};

    tb_platform_atomic_size_store(&atomic, 1);
}

TB_TEST(platform_default, tb_platform_atomic_size_exchange)
{
    tb_platform_atomic_size atomic = {0};

    (void)tb_platform_atomic_size_exchange(&atomic, 1);
}

TB_TEST(platform_default, tb_platform_atomic_size_compare_exchange)
{
    tb_platform_atomic_size atomic = {0};
    size_t expected = 0;

    (void)tb_platform_atomic_size_compare_exchange(&atomic, &expected, 1);
}

TB_TEST(platform_default, tb_platform_atomic_u32_load)
{
    tb_platform_atomic_u32 atomic = {0};

    (void)tb_platform_atomic_u32_load(&atomic);
}

TB_TEST(platform_default, tb_platform_atomic_u32_store)
{
    tb_platform_atomic_u32 atomic = {0};

    tb_platform_atomic_u32_store(&atomic, 1);
}

TB_TEST(platform_default, tb_platform_atomic_u32_exchange)
{
    tb_platform_atomic_u32 atomic = {0};

    (void)tb_platform_atomic_u32_exchange(&atomic, 1);
}

TB_TEST(platform_default, tb_platform_atomic_u32_compare_exchange)
{
    tb_platform_atomic_u32 atomic = {0};
    uint32_t expected = 0;

    (void)tb_platform_atomic_u32_compare_exchange(&atomic, &expected, 1);
}

TB_TEST(platform_default, tb_platform_atomic_pointer_load)
{
    tb_platform_atomic_pointer atomic = {0};

    (void)tb_platform_atomic_pointer_load(&atomic);
}

TB_TEST(platform_default, tb_platform_atomic_pointer_store)
{
    tb_platform_atomic_pointer atomic = {0};

    tb_platform_atomic_pointer_store(&atomic, NULL);
}

TB_TEST(platform_default, tb_platform_atomic_pointer_exchange)
{
    tb_platform_atomic_pointer atomic = {0};

    (void)tb_platform_atomic_pointer_exchange(&atomic, NULL);
}

TB_TEST(platform_default, tb_platform_atomic_pointer_compare_exchange)
{
    tb_platform_atomic_pointer atomic = {0};
    void *expected = NULL;

    (void)tb_platform_atomic_pointer_compare_exchange(&atomic, &expected, NULL);
}

TB_TEST(platform_default, tb_random_bytes)
{
    unsigned char byte = 0;

    (void)tb_random_bytes(&byte, 1);
}

TB_TEST(platform_default, tb_system_get_processors)
{
    tb_system_processors processors = {0};

    tb_system_get_processors(&processors);
}

TB_TEST(platform_default, tb_system_list_processors)
{
    (void)tb_system_list_processors(NULL, 0);
}

TB_TEST(platform_default, tb_thread_create)
{
    tb_thread thread = {0};

    tb_thread_create(&thread, do_nothing, NULL);
}

TB_TEST(platform_default, tb_thread_create_with_options)
{
    tb_thread thread = {0};

    tb_thread_create_with_options(&thread, NULL, do_nothing, NULL);
}

TB_TEST(platform_default, tb_thread_join)
{
    tb_thread thread = {0};

    tb_thread_join(&thread);
}

TB_TEST(platform_default, tb_thread_current_id)
{
    (void)tb_thread_current_id();
}

TB_TEST(platform_default, tb_thread_current_name)
{
    char name[TB_THREAD_NAME_CAPACITY] = {0};

    (void)tb_thread_current_name(name, sizeof(name));
}

TB_TEST(platform_default, tb_thread_can_be_confined)
{
    (void)tb_thread_can_be_confined();
}

TB_TEST(platform_default, tb_thread_current_processor)
{
    (void)tb_thread_current_processor();
}

TB_TEST(platform_default, tb_mutex_lock)
{
    tb_mutex mutex = {0};

    tb_mutex_lock(&mutex);
}

TB_TEST(platform_default, tb_mutex_try_lock)
{
    tb_mutex mutex = {0};

    (void)tb_mutex_try_lock(&mutex);
}

TB_TEST(platform_default, tb_mutex_unlock)
{
    tb_mutex mutex = {0};

    tb_mutex_unlock(&mutex);
}

TB_TEST(platform_default, tb_condition_wait)
{
    tb_condition condition = {0};
    tb_mutex mutex = {0};

    tb_condition_wait(&condition, &mutex);
}

TB_TEST(platform_default, tb_condition_wait_for)
{
    tb_condition condition = {0};
    tb_mutex mutex = {0};

    (void)tb_condition_wait_for(&condition, &mutex, 1);
}

TB_TEST(platform_default, tb_condition_wake_one)
{
    tb_condition condition = {0};

    tb_condition_wake_one(&condition);
}

TB_TEST(platform_default, tb_condition_wake_all)
{
    tb_condition condition = {0};

    tb_condition_wake_all(&condition);
}

TB_TEST(platform_default, tb_semaphore_signal)
{
    tb_semaphore semaphore = {0};

    tb_semaphore_signal(&semaphore, 1);
}

TB_TEST(platform_default, tb_semaphore_wait)
{
    tb_semaphore semaphore = {0};

    tb_semaphore_wait(&semaphore);
}

TB_TEST(platform_default, tb_semaphore_try_wait)
{
    tb_semaphore semaphore = {0};

    (void)tb_semaphore_try_wait(&semaphore);
}

TB_TEST(platform_default, tb_semaphore_wait_for)
{
    tb_semaphore semaphore = {0};

    (void)tb_semaphore_wait_for(&semaphore, 0);
}

TB_TEST(platform_default, tb_thread_local_create)
{
    tb_thread_local local = {0};

    tb_thread_local_create(&local, NULL);
}

TB_TEST(platform_default, tb_thread_local_destroy)
{
    tb_thread_local local = {0};

    tb_thread_local_destroy(&local);
}

TB_TEST(platform_default, tb_thread_local_get)
{
    tb_thread_local local = {0};

    (void)tb_thread_local_get(&local);
}

TB_TEST(platform_default, tb_thread_local_set)
{
    tb_thread_local local = {0};

    tb_thread_local_set(&local, NULL);
}

TB_TEST(platform_default, tb_thread_relax)
{
    tb_thread_relax();
}

TB_TEST(platform_default, tb_thread_yield)
{
    tb_thread_yield();
}

#include "badger/file.h"

TB_TEST(platform_default, tb_file_open)
{
    (void)tb_file_open((tb_path_view){0}, 0, NULL);
}

TB_TEST(platform_default, tb_file_read)
{
    (void)tb_file_read(NULL, NULL, 0, NULL);
}

TB_TEST(platform_default, tb_file_write)
{
    (void)tb_file_write(NULL, NULL, 0, NULL);
}

TB_TEST(platform_default, tb_file_read_at)
{
    (void)tb_file_read_at(NULL, 0, NULL, 0, NULL);
}

TB_TEST(platform_default, tb_file_write_at)
{
    (void)tb_file_write_at(NULL, 0, NULL, 0, NULL);
}

TB_TEST(platform_default, tb_file_seek)
{
    (void)tb_file_seek(NULL, 0, 0, NULL);
}

TB_TEST(platform_default, tb_file_tell)
{
    (void)tb_file_tell(NULL, NULL);
}

TB_TEST(platform_default, tb_file_size)
{
    (void)tb_file_size(NULL, NULL);
}

TB_TEST(platform_default, tb_file_flush)
{
    (void)tb_file_flush(NULL);
}

TB_TEST(platform_default, tb_file_close)
{
    (void)tb_file_close(NULL);
}

TB_TEST(platform_default, tb_fs_query)
{
    (void)tb_fs_query((tb_path_view){0}, NULL);
}

TB_TEST(platform_default, tb_fs_make_directory)
{
    (void)tb_fs_make_directory((tb_path_view){0});
}

TB_TEST(platform_default, tb_fs_remove)
{
    (void)tb_fs_remove((tb_path_view){0});
}

TB_TEST(platform_default, tb_fs_rename)
{
    (void)tb_fs_rename((tb_path_view){0}, (tb_path_view){0});
}

TB_TEST(platform_default, tb_directory_open)
{
    (void)tb_directory_open((tb_path_view){0}, NULL);
}

TB_TEST(platform_default, tb_directory_next)
{
    (void)tb_directory_next(NULL, NULL, 0, NULL);
}

TB_TEST(platform_default, tb_directory_close)
{
    (void)tb_directory_close(NULL);
}

TB_TEST(platform_default, tb_fs_get_location)
{
    (void)tb_fs_get_location(0, NULL, 0, NULL);
}

TB_TEST(platform_default, tb_fs_temp_file)
{
    (void)tb_fs_temp_file((tb_path_view){0}, NULL, 0, NULL);
}

TB_TEST(platform_default, tb_fs_temp_directory)
{
    (void)tb_fs_temp_directory((tb_path_view){0}, NULL, 0);
}

TB_TEST(platform_default, tb_fs_native_style)
{
    (void)tb_fs_native_style();
}

#include "badger/virtual_memory.h"

TB_TEST(platform_default, tb_vm_get_info)
{
    (void)tb_vm_get_info();
}

TB_TEST(platform_default, tb_vm_reserve)
{
    (void)tb_vm_reserve(0, NULL);
}

TB_TEST(platform_default, tb_vm_commit)
{
    (void)tb_vm_commit(NULL, 0, 0);
}

TB_TEST(platform_default, tb_vm_decommit)
{
    (void)tb_vm_decommit(NULL, 0, 0);
}

TB_TEST(platform_default, tb_vm_release)
{
    (void)tb_vm_release(NULL);
}

TB_TEST(platform_default, tb_vm_allocate_large)
{
    (void)tb_vm_allocate_large(0, NULL);
}

TB_TEST(platform_default, tb_window_system_open)
{
    tb_window_system *system = NULL;

    (void)tb_window_system_open(NULL, &system, NULL);
}

TB_TEST(platform_default, tb_window_system_close)
{
    tb_window_system_close(NULL);
}

TB_TEST(platform_default, tb_window_system_backend)
{
    (void)tb_window_system_backend(NULL);
}

TB_TEST(platform_default, tb_window_system_display_count)
{
    (void)tb_window_system_display_count(NULL);
}

TB_TEST(platform_default, tb_window_system_display)
{
    tb_display display = {0};

    tb_window_system_display(NULL, 0, &display);
}

TB_TEST(platform_default, tb_window_system_poll)
{
    tb_window_event event = {0};

    (void)tb_window_system_poll(NULL, &event);
}

TB_TEST(platform_default, tb_window_system_wait)
{
    tb_window_system_wait(NULL, 0);
}

TB_TEST(platform_default, tb_window_system_wake)
{
    tb_window_system_wake(NULL);
}

TB_TEST(platform_default, tb_window_create)
{
    tb_window *window = NULL;

    (void)tb_window_create(NULL, NULL, &window, NULL);
}

TB_TEST(platform_default, tb_window_destroy)
{
    tb_window_destroy(NULL);
}

TB_TEST(platform_default, tb_window_configure)
{
    const tb_window_options options = {0};

    (void)tb_window_configure(NULL, &options, NULL);
}

TB_TEST(platform_default, tb_window_state_of)
{
    tb_window_state state = {0};

    tb_window_state_of(NULL, &state);
}

TB_TEST(platform_default, tb_window_native_of)
{
    tb_window_native native = {0};

    tb_window_native_of(NULL, &native);
}

TB_TEST(platform_default, tb_window_backend_name)
{
    (void)tb_window_backend_name(TB_WINDOW_BACKEND_AUTOMATIC);
}

TB_TEST(platform_default, tb_key_name)
{
    (void)tb_key_name(TB_KEY_ESCAPE);
}
