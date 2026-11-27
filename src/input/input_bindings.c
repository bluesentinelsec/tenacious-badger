/* Bindings files: a player's rebinds, as differences from the defaults
 * (docs/input.md), read all or nothing and written back the same. */
#include "input_internal.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/json.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/vfs.h"

#include <string.h>

/* The deepest a bindings file nests: a binding in the list in the file. */
#define TB_INPUT_BINDINGS_JSON_DEPTH 4u
/* Bytes of an index written in a path: "[18446744073709551615]". */
#define TB_INPUT_BINDINGS_INDEX_TEXT 24u

/* A rebind read, until the file is known to be whole. */
typedef struct tb_input_read_rebind
{
    tb_input_context context;
    size_t mapping;
    tb_input_key key;
} tb_input_read_rebind;

typedef struct tb_input_bindings_reader
{
    tb_input_player *player;
    const tb_input *input;
    const char *text;
    size_t length;
    const char *file_name;
    tb_input_file_error *error;
    char path[TB_INPUT_FILE_ERROR_CAPACITY];
    tb_input_read_rebind *rebinds;
    size_t rebind_count;
} tb_input_bindings_reader;

/* Fail at a value: the path is the reader's, with `member` after it when
 * not NULL. */
static tb_input_file_result tb_input_bindings_fail(tb_input_bindings_reader *reader,
                                                   const tb_json_value *value, const char *member,
                                                   const char *reason, const char *detail)
{
    tb_input_file_error *const error = reader->error;
    const size_t offset = tb_json_offset_of(value);
    size_t line_start = 0;

    *error = (tb_input_file_error){.result = TB_INPUT_FILE_INVALID, .reason = reason, .line = 1};
    if (member != NULL)
    {
        (void)tb_fmt_buffer(error->path, sizeof(error->path), "%s.%s", reader->path, member);
    }
    else
    {
        (void)tb_fmt_buffer(error->path, sizeof(error->path), "%s", reader->path);
    }
    for (size_t index = 0; index < offset && index < reader->length; index++)
    {
        if (reader->text[index] == '\n')
        {
            error->line++;
            line_start = index + 1;
        }
    }
    error->column = offset - line_start + 1;
    tb_input_error_message(error, reader->file_name, detail);
    return TB_INPUT_FILE_INVALID;
}

/* Every member of an object is one of `names`. */
static tb_input_file_result tb_input_bindings_known(tb_input_bindings_reader *reader,
                                                    const tb_json_value *object,
                                                    const char *const *names, size_t count)
{
    for (size_t member = 0; member < tb_json_count(object); member++)
    {
        const char *key = NULL;
        size_t length = 0;
        const tb_json_value *const value = tb_json_member_at(object, member, &key, &length);
        bool known = false;
        for (size_t index = 0; index < count; index++)
        {
            known = known || (length == tb_str_length(names[index]) &&
                              memcmp(names[index], key, length) == 0);
        }
        if (!known)
        {
            return tb_input_bindings_fail(reader, value, key, "a key the format does not have",
                                          key);
        }
    }
    return TB_INPUT_FILE_OK;
}

/* A string member, required. */
static tb_input_file_result tb_input_bindings_string(tb_input_bindings_reader *reader,
                                                     const tb_json_value *object,
                                                     const char *member, const char **text)
{
    const tb_json_value *const value = tb_json_find(object, member, tb_str_length(member));

    *text = tb_input_text_of(value);
    if (*text == NULL)
    {
        return tb_input_bindings_fail(reader, value != NULL ? value : object, member, "a string",
                                      NULL);
    }
    return TB_INPUT_FILE_OK;
}

/* A key's name, or for `"key"`, null for none. */
static tb_input_file_result tb_input_bindings_key(tb_input_bindings_reader *reader,
                                                  const tb_json_value *object, const char *member,
                                                  bool null_is_none, tb_input_key *key)
{
    const tb_json_value *const value = tb_json_find(object, member, tb_str_length(member));
    const char *name = NULL;

    if (null_is_none && value != NULL && tb_json_type_of(value) == TB_JSON_NULL)
    {
        *key = TB_INPUT_KEY_NONE;
        return TB_INPUT_FILE_OK;
    }
    const tb_input_file_result result = tb_input_bindings_string(reader, object, member, &name);
    if (result != TB_INPUT_FILE_OK)
    {
        return result;
    }
    *key = tb_input_find_key(name, tb_str_length(name));
    if (*key == TB_INPUT_KEY_NONE)
    {
        return tb_input_bindings_fail(reader, value, member, "an unknown key", name);
    }
    return TB_INPUT_FILE_OK;
}

/* A binding's slot: a whole number from 0; 0 when left out. */
static tb_input_file_result tb_input_bindings_slot(tb_input_bindings_reader *reader,
                                                   const tb_json_value *object, uint64_t *slot)
{
    const tb_json_value *const value = tb_json_find(object, "slot", 4);

    *slot = 0;
    if (value != NULL && !tb_json_as_uint64(value, slot))
    {
        return tb_input_bindings_fail(reader, value, "slot", "a slot: a whole number from 0", NULL);
    }
    return TB_INPUT_FILE_OK;
}

/* The mapping a binding names: of its context's mappings of its action
 * from its default key, the `slot`-th, from 0. */
static bool tb_input_bindings_find(const tb_input_bindings_reader *reader, const char *context_name,
                                   const char *action_name, tb_input_key fallback, uint64_t slot,
                                   tb_input_context *context, size_t *mapping)
{
    const tb_input_action action = tb_input_find_action(reader->input, action_name);
    uint64_t seen = 0;

    *context = tb_input_find_context(reader->input, context_name);
    if (context->id == 0 || action.id == 0)
    {
        return false;
    }
    for (size_t index = 0; index < tb_input_context_mapping_count(reader->input, *context); index++)
    {
        tb_input_mapping_info info = {0};
        tb_input_context_mapping_at(reader->input, *context, index, &info);
        if (info.action.id != action.id || info.key != fallback)
        {
            continue;
        }
        if (seen == slot)
        {
            *mapping = index;
            return true;
        }
        seen++;
    }
    return false;
}

static tb_input_file_result tb_input_bindings_read_one(tb_input_bindings_reader *reader,
                                                       const tb_json_value *object)
{
    static const char *const names[] = {"context", "action", "default", "slot", "key"};
    const char *context_name = NULL;
    const char *action_name = NULL;
    tb_input_key fallback = TB_INPUT_KEY_NONE;
    tb_input_key key = TB_INPUT_KEY_NONE;
    tb_input_context context = TB_INPUT_CONTEXT_NONE;
    size_t mapping = 0;
    uint64_t slot = 0;

    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_input_bindings_fail(reader, object, NULL, "a binding: an object", NULL);
    }
    tb_input_file_result result = tb_input_bindings_known(reader, object, names, 5);
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_bindings_string(reader, object, "context", &context_name);
    }
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_bindings_string(reader, object, "action", &action_name);
    }
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_bindings_key(reader, object, "default", false, &fallback);
    }
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_bindings_slot(reader, object, &slot);
    }
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_bindings_key(reader, object, "key", true, &key);
    }
    if (result != TB_INPUT_FILE_OK)
    {
        return result;
    }
    /* A binding of a mapping the input no longer has is passed over. */
    if (!tb_input_bindings_find(reader, context_name, action_name, fallback, slot, &context,
                                &mapping))
    {
        return TB_INPUT_FILE_OK;
    }
    reader->rebinds[reader->rebind_count] =
        (tb_input_read_rebind){.context = context, .mapping = mapping, .key = key};
    reader->rebind_count++;
    return TB_INPUT_FILE_OK;
}

static tb_input_file_result tb_input_bindings_read_head(tb_input_bindings_reader *reader,
                                                        const tb_json_value *root)
{
    static const char *const names[] = {"format", "version", "bindings"};
    const tb_json_value *const version = tb_json_find(root, "version", 7);
    int64_t number = 0;

    if (tb_json_type_of(root) != TB_JSON_OBJECT)
    {
        return tb_input_bindings_fail(reader, root, NULL, "bindings: an object", NULL);
    }
    const tb_input_file_result result = tb_input_bindings_known(reader, root, names, 3);
    if (result != TB_INPUT_FILE_OK)
    {
        return result;
    }
    const char *const format = tb_input_text_of(tb_json_find(root, "format", 6));
    if (format == NULL || !tb_str_equal(format, TB_INPUT_BINDINGS_FORMAT))
    {
        const tb_json_value *const found = tb_json_find(root, "format", 6);
        return tb_input_bindings_fail(reader, found != NULL ? found : root, "format",
                                      "a format other than \"" TB_INPUT_BINDINGS_FORMAT "\"", NULL);
    }
    if (version == NULL || !tb_json_as_int64(version, &number) || number < 1)
    {
        return tb_input_bindings_fail(reader, version != NULL ? version : root, "version",
                                      "a version: a whole number from 1", NULL);
    }
    if (number > TB_INPUT_BINDINGS_VERSION)
    {
        return tb_input_bindings_fail(reader, version, "version",
                                      "a newer version than this engine reads", NULL);
    }
    return TB_INPUT_FILE_OK;
}

static tb_input_file_result tb_input_bindings_read(tb_input_bindings_reader *reader,
                                                   const tb_json_value *root)
{
    tb_input_file_result result = tb_input_bindings_read_head(reader, root);

    if (result != TB_INPUT_FILE_OK)
    {
        return result;
    }
    const tb_json_value *const list = tb_json_find(root, "bindings", 8);
    if (list == NULL)
    {
        return TB_INPUT_FILE_OK;
    }
    if (tb_json_type_of(list) != TB_JSON_ARRAY)
    {
        return tb_input_bindings_fail(reader, list, "bindings", "an array", NULL);
    }
    reader->rebinds = tb_calloc(tb_json_count(list) + 1, sizeof(*reader->rebinds));
    for (size_t index = 0; index < tb_json_count(list); index++)
    {
        (void)tb_fmt_buffer(reader->path, sizeof(reader->path), "$.bindings[%zu]", index);
        result = tb_input_bindings_read_one(reader, tb_json_at(list, index));
        if (result != TB_INPUT_FILE_OK)
        {
            return result;
        }
    }
    return TB_INPUT_FILE_OK;
}

tb_input_file_result tb_input_load_bindings(tb_input_player *player, const char *text,
                                            size_t length, const char *name,
                                            tb_input_file_error *error)
{
    tb_input_file_error ignored = {0};
    tb_input_bindings_reader reader = {.player = player,
                                       .text = text,
                                       .length = length,
                                       .file_name = name,
                                       .error = error != NULL ? error : &ignored,
                                       .path = "$"};
    const tb_json_options options = {.max_depth = TB_INPUT_BINDINGS_JSON_DEPTH};
    tb_json_error json_error = {0};
    const tb_json_value *root = NULL;
    tb_arena *const arena = tb_arena_create(0);
    tb_input_file_result result = TB_INPUT_FILE_OK;

    TB_ASSERT(player != NULL);
    TB_ASSERT(text != NULL || length == 0);
    reader.input = tb_input_player_input(player);
    *reader.error = (tb_input_file_error){.reason = ""};
    if (tb_json_parse(text, length, &options, arena, &root, &json_error) != TB_JSON_OK)
    {
        *reader.error = (tb_input_file_error){.result = TB_INPUT_FILE_MALFORMED,
                                              .line = json_error.line,
                                              .column = json_error.column,
                                              .reason = json_error.reason};
        tb_input_error_message(reader.error, name, NULL);
        result = TB_INPUT_FILE_MALFORMED;
        goto cleanup;
    }
    result = tb_input_bindings_read(&reader, root);
    if (result != TB_INPUT_FILE_OK)
    {
        goto cleanup;
    }
    tb_input_reset_bindings(player);
    for (size_t index = 0; index < reader.rebind_count; index++)
    {
        const tb_input_read_rebind *const rebind = &reader.rebinds[index];
        tb_input_rebind(player, rebind->context, rebind->mapping, rebind->key);
    }
cleanup:
    tb_free(reader.rebinds);
    tb_arena_destroy(arena);
    return result;
}

tb_input_file_result tb_input_load_bindings_file(tb_input_player *player, tb_vfs *vfs,
                                                 const char *path, tb_input_file_error *error)
{
    tb_input_file_error ignored = {0};
    tb_input_file_error *const found = error != NULL ? error : &ignored;
    char *text = NULL;
    size_t length = 0;

    TB_ASSERT(player != NULL);
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(path != NULL);
    tb_input_file_result result = tb_input_read_vfs_file(vfs, path, found, &text, &length);
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_load_bindings(player, text, length, path, found);
    }
    tb_free(text);
    return result;
}

/* ---------------------------------------------------------------------------
 * The writer.
 * ------------------------------------------------------------------------- */

typedef struct tb_input_bindings_writer
{
    tb_json_writer *json;
    bool failed;
} tb_input_bindings_writer;

static void tb_input_bindings_check(tb_input_bindings_writer *writer, tb_json_result result)
{
    writer->failed = writer->failed || result != TB_JSON_OK;
}

static void tb_input_bindings_write(tb_input_bindings_writer *writer, const char *key,
                                    const char *text)
{
    tb_input_bindings_check(writer, tb_json_write_key(writer->json, key, tb_str_length(key)));
    if (text == NULL)
    {
        tb_input_bindings_check(writer, tb_json_write_null(writer->json));
        return;
    }
    tb_input_bindings_check(writer, tb_json_write_string(writer->json, text, tb_str_length(text)));
}

/* One rebind: its mapping named by context, action and default key, and
 * among the context's mappings of that action from that key, its slot. */
static void tb_input_bindings_write_one(tb_input_bindings_writer *writer, const tb_input *input,
                                        const tb_input_player *player, tb_input_context context,
                                        size_t mapping)
{
    tb_input_mapping_info info = {0};
    const tb_input_key key = tb_input_binding_key(player, context, mapping);

    uint64_t slot = 0;

    tb_input_context_mapping_at(input, context, mapping, &info);
    for (size_t index = 0; index < mapping; index++)
    {
        tb_input_mapping_info before = {0};
        tb_input_context_mapping_at(input, context, index, &before);
        if (before.action.id == info.action.id && before.key == info.key)
        {
            slot++;
        }
    }
    tb_input_bindings_check(writer, tb_json_write_begin_object(writer->json));
    tb_input_bindings_write(writer, "context", tb_input_context_name(input, context));
    tb_input_bindings_write(writer, "action", tb_input_action_name(input, info.action));
    tb_input_bindings_write(writer, "default", tb_input_key_name(info.key));
    if (slot > 0)
    {
        tb_input_bindings_check(writer, tb_json_write_key(writer->json, "slot", 4));
        tb_input_bindings_check(writer, tb_json_write_uint64(writer->json, slot));
    }
    tb_input_bindings_write(writer, "key",
                            key != TB_INPUT_KEY_NONE ? tb_input_key_name(key) : NULL);
    tb_input_bindings_check(writer, tb_json_write_end_object(writer->json));
}

tb_input_file_result tb_input_save_bindings(const tb_input_player *player, tb_stream *sink,
                                            tb_input_file_error *error)
{
    tb_input_file_error ignored = {0};
    tb_input_file_error *const found = error != NULL ? error : &ignored;
    tb_input_bindings_writer writer = {0};

    TB_ASSERT(player != NULL);
    TB_ASSERT(sink != NULL);
    const tb_input *const input = tb_input_player_input(player);
    *found = (tb_input_file_error){.reason = ""};
    writer.json = tb_json_writer_create(sink, TB_JSON_INDENTED, NULL);
    tb_input_bindings_check(&writer, tb_json_write_begin_object(writer.json));
    tb_input_bindings_write(&writer, "format", TB_INPUT_BINDINGS_FORMAT);
    tb_input_bindings_check(&writer, tb_json_write_key(writer.json, "version", 7));
    tb_input_bindings_check(&writer, tb_json_write_uint64(writer.json, TB_INPUT_BINDINGS_VERSION));
    tb_input_bindings_check(&writer, tb_json_write_key(writer.json, "bindings", 8));
    tb_input_bindings_check(&writer, tb_json_write_begin_array(writer.json));
    /* In the order of contexts, then of mappings: the same rebinds, the
     * same bytes. */
    for (size_t index = 0; index < tb_input_context_count(input); index++)
    {
        const tb_input_context context = tb_input_context_at(input, index);
        for (size_t mapping = 0; mapping < tb_input_context_mapping_count(input, context);
             mapping++)
        {
            if (tb_input_is_rebound(player, context, mapping))
            {
                tb_input_bindings_write_one(&writer, input, player, context, mapping);
            }
        }
    }
    tb_input_bindings_check(&writer, tb_json_write_end_array(writer.json));
    tb_input_bindings_check(&writer, tb_json_write_end_object(writer.json));
    tb_input_bindings_check(&writer, tb_json_writer_finish(writer.json));
    tb_json_writer_destroy(writer.json);
    if (writer.failed)
    {
        return tb_input_fail_plain(found, TB_INPUT_FILE_OUTPUT_FAILED, NULL,
                                   "the stream refused what was written");
    }
    return TB_INPUT_FILE_OK;
}
