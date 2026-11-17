/* Independent installed-SDK consumer: paths are read from a UTF-8 request file,
 * so the example does not assume any encoding for Windows narrow argv. */
#include "badger/arena.h"
#include "badger/fs.h"
#include "badger/log.h"
#include "badger/size.h"
#include "badger/str.h"
#include "badger/utf8.h"
#include <stdlib.h>
#include <string.h>

static bool parse_size(const char *text, size_t *value)
{
    size_t result = {0};
    if (text[0] == '\0')
    {
        return false;
    }
    for (size_t index = {0}; text[index] != '\0'; index++)
    {
        if (text[index] < '0' || text[index] > '9' || !tb_size_multiply(result, 10, &result) ||
            !tb_size_add(result, (size_t)(text[index] - '0'), &result))
        {
            return false;
        }
    }
    *value = result;
    return true;
}
static bool parse_request(tb_fs_buffer *request, tb_path_view *source, tb_path_view *destination)
{
    char *const bytes = (char *)request->data;
    size_t error = {0};
    size_t split = {0};
    size_t end = {0};
    tb_path_parts parts = {0};
    if (!tb_utf8_validate(bytes, request->length, &error) ||
        memchr(bytes, 0, request->length) != NULL)
    {
        return false;
    }
    while (split < request->length && bytes[split] != '\n')
    {
        split++;
    }
    if (split == request->length)
    {
        return false;
    }
    end = request->length;
    if (end > split + 1 && bytes[end - 1] == '\n')
    {
        end--;
    }
    if (end > split + 1 && bytes[end - 1] == '\r')
    {
        end--;
    }
    *source = (tb_path_view){bytes, split};
    if (source->length > 0 && bytes[source->length - 1] == '\r')
    {
        source->length--;
    }
    *destination = (tb_path_view){bytes + split + 1, end - split - 1};
    if (source->length == 0 || destination->length == 0 ||
        memchr(destination->data, '\n', destination->length) != NULL ||
        memchr(source->data, '\r', source->length) != NULL ||
        memchr(destination->data, '\r', destination->length) != NULL)
    {
        return false;
    }
    /* Parse without rewriting: symlink/.. resolution belongs to the native FS. */
    return tb_path_parse(tb_fs_native_style(), *source, &parts) == TB_PATH_OK &&
           parts.name.length != 0 &&
           tb_path_parse(tb_fs_native_style(), *destination, &parts) == TB_PATH_OK &&
           parts.name.length != 0;
}
int main(int argc, char **argv)
{
    tb_log *log = NULL;
    tb_arena *arena = NULL;
    tb_allocator allocator = {0};
    tb_fs_buffer request = {0};
    tb_fs_buffer content = {0};
    tb_path_view source = {0};
    tb_path_view destination = {0};
    size_t maximum = (size_t)16 * 1024 * 1024;
    size_t reservation = {0};
    tb_fs_result result = {0};
    int status = EXIT_FAILURE;
    log = tb_log_create();
    if (argc < 2 || argc > 4 || (argc > 2 && !parse_size(argv[2], &maximum)) ||
        (argc > 3 && !parse_size(argv[3], &reservation)))
    {
        (void)tb_log_printf(log, TB_LOG_ERROR, "copy",
                            "usage: foundation_copy request.txt [maximum-bytes] [reserve-bytes]");
        goto cleanup;
    }
    /* Only the bootstrap filename uses argv. The request contains native UTF-8
     * source/destination paths, including Unicode on every platform. */
    for (size_t index = {0}; argv[1][index] != '\0'; index++)
    {
        if ((unsigned char)argv[1][index] > 127)
        {
            (void)tb_log_printf(log, TB_LOG_ERROR, "copy",
                                "use an ASCII request filename; place UTF-8 paths inside it");
            goto cleanup;
        }
    }
    result =
        tb_fs_read_file((tb_path_view){argv[1], tb_str_length(argv[1])}, NULL, 65536, &request);
    if (result != TB_FS_OK || !parse_request(&request, &source, &destination))
    {
        (void)tb_log_printf(log, TB_LOG_ERROR, "copy", "invalid or unreadable UTF-8 request");
        goto cleanup;
    }
    arena = tb_arena_try_create_reserved(reservation);
    if (arena == NULL)
    {
        (void)tb_log_printf(log, TB_LOG_ERROR, "copy", "reservation failed");
        goto cleanup;
    }
    allocator = tb_arena_allocator(arena);
    result = tb_fs_read_file(source, &allocator, maximum, &content);
    if (result != TB_FS_OK)
    {
        (void)tb_log_printf(log, TB_LOG_ERROR, "copy", "read failed: %u", (unsigned)result);
        goto cleanup;
    }
    result = tb_fs_replace_file(destination, content.data, content.length);
    if (result != TB_FS_OK)
    {
        (void)tb_log_printf(log, TB_LOG_ERROR, "copy", "replacement failed: %u", (unsigned)result);
        goto cleanup;
    }
    if (tb_log_printf(log, TB_LOG_INFO, "copy",
                      "copied %zu bytes; committed %zu of %zu reserved bytes", content.length,
                      tb_arena_committed_size(arena), tb_arena_reserved_size(arena)) != TB_LOG_OK)
    {
        goto cleanup;
    }
    status = EXIT_SUCCESS;
cleanup:
    tb_fs_buffer_free(&content);
    tb_arena_destroy(arena);
    tb_fs_buffer_free(&request);
    if (tb_log_destroy(log) != TB_LOG_OK)
    {
        status = EXIT_FAILURE;
    }
    return status;
}
