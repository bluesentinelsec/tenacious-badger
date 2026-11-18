#include "badger/file.h"
#include "not_implemented.h"

tb_fs_result tb_file_open(tb_path_view path, unsigned flags, tb_file *file)
{
    (void)path;
    (void)flags;
    (void)file;
    tb_default_not_implemented("platform.files", "tb_file_open");
}

tb_fs_result tb_file_read(tb_file *file, void *bytes, size_t capacity, size_t *transferred)
{
    (void)file;
    (void)bytes;
    (void)capacity;
    (void)transferred;
    tb_default_not_implemented("platform.files", "tb_file_read");
}

tb_fs_result tb_file_write(tb_file *file, const void *bytes, size_t length, size_t *transferred)
{
    (void)file;
    (void)bytes;
    (void)length;
    (void)transferred;
    tb_default_not_implemented("platform.files", "tb_file_write");
}

tb_fs_result tb_file_read_at(tb_file *file, uint64_t offset, void *bytes, size_t capacity,
                             size_t *transferred)
{
    (void)file;
    (void)offset;
    (void)bytes;
    (void)capacity;
    (void)transferred;
    tb_default_not_implemented("platform.files", "tb_file_read_at");
}

tb_fs_result tb_file_write_at(tb_file *file, uint64_t offset, const void *bytes, size_t length,
                              size_t *transferred)
{
    (void)file;
    (void)offset;
    (void)bytes;
    (void)length;
    (void)transferred;
    tb_default_not_implemented("platform.files", "tb_file_write_at");
}

tb_fs_result tb_file_seek(tb_file *file, int64_t offset, tb_file_origin origin, uint64_t *position)
{
    (void)file;
    (void)offset;
    (void)origin;
    (void)position;
    tb_default_not_implemented("platform.files", "tb_file_seek");
}

tb_fs_result tb_file_tell(tb_file *file, uint64_t *position)
{
    (void)file;
    (void)position;
    tb_default_not_implemented("platform.files", "tb_file_tell");
}

tb_fs_result tb_file_size(tb_file *file, uint64_t *size)
{
    (void)file;
    (void)size;
    tb_default_not_implemented("platform.files", "tb_file_size");
}

tb_fs_result tb_file_flush(tb_file *file)
{
    (void)file;
    tb_default_not_implemented("platform.files", "tb_file_flush");
}

tb_fs_result tb_file_close(tb_file *file)
{
    (void)file;
    tb_default_not_implemented("platform.files", "tb_file_close");
}

tb_fs_result tb_fs_query(tb_path_view path, tb_fs_info *info)
{
    (void)path;
    (void)info;
    tb_default_not_implemented("platform.files", "tb_fs_query");
}

tb_fs_result tb_fs_make_directory(tb_path_view path)
{
    (void)path;
    tb_default_not_implemented("platform.files", "tb_fs_make_directory");
}

tb_fs_result tb_fs_remove(tb_path_view path)
{
    (void)path;
    tb_default_not_implemented("platform.files", "tb_fs_remove");
}

tb_fs_result tb_fs_rename(tb_path_view source, tb_path_view destination)
{
    (void)source;
    (void)destination;
    tb_default_not_implemented("platform.files", "tb_fs_rename");
}

tb_fs_result tb_directory_open(tb_path_view path, tb_directory *directory)
{
    (void)path;
    (void)directory;
    tb_default_not_implemented("platform.files", "tb_directory_open");
}

tb_fs_result tb_directory_next(tb_directory *directory, char *name, size_t capacity,
                               size_t *required)
{
    (void)directory;
    (void)name;
    (void)capacity;
    (void)required;
    tb_default_not_implemented("platform.files", "tb_directory_next");
}

tb_fs_result tb_directory_close(tb_directory *directory)
{
    (void)directory;
    tb_default_not_implemented("platform.files", "tb_directory_close");
}

tb_fs_result tb_fs_get_location(tb_fs_location location, char *destination, size_t capacity,
                                size_t *required)
{
    (void)location;
    (void)destination;
    (void)capacity;
    (void)required;
    tb_default_not_implemented("platform.files", "tb_fs_get_location");
}

tb_fs_result tb_fs_temp_file(tb_path_view directory, char *path, size_t capacity, tb_file *file)
{
    (void)directory;
    (void)path;
    (void)capacity;
    (void)file;
    tb_default_not_implemented("platform.files", "tb_fs_temp_file");
}

tb_fs_result tb_fs_temp_directory(tb_path_view directory, char *path, size_t capacity)
{
    (void)directory;
    (void)path;
    (void)capacity;
    tb_default_not_implemented("platform.files", "tb_fs_temp_directory");
}

tb_path_style tb_fs_native_style(void)
{
    tb_default_not_implemented("platform.files", "tb_fs_native_style");
}
