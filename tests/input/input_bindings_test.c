/* Bindings files hold a player's rebinds as differences from the
 * defaults: written in a stable order and read back to the same, all or
 * nothing, passing over rebinds of mappings a game no longer has, so that
 * new defaults never refuse a player's old file. */
#include "badger/input.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/vfs.h"

#include "tb_test.h"

#include <stddef.h>
#include <string.h>

#define BINDINGS_PATH "save/bindings.json"

static const char definitions[] =
    "{\"format\": \"badger-input\", \"version\": 1,\n"
    " \"actions\": [{\"name\": \"jump\", \"type\": \"bool\"}, {\"name\": \"fire\", \"type\": "
    "\"bool\"}],\n"
    " \"contexts\": [\n"
    "  {\"name\": \"gameplay\", \"mappings\": [\n"
    "   {\"action\": \"jump\", \"key\": \"space\"},\n"
    "   {\"action\": \"jump\", \"key\": \"gamepad_face_south\"},\n"
    "   {\"action\": \"fire\", \"key\": \"mouse_left\"},\n"
    "   {\"action\": \"fire\", \"key\": \"mouse_left\"}]},\n"
    "  {\"name\": \"menu\", \"mappings\": [{\"action\": \"jump\", \"key\": \"enter\"}]}]}\n";

static tb_input *make_input(void)
{
    tb_input *const input = tb_input_create(NULL);

    TB_TEST_REQUIRE_UINT(tb_input_load(input, definitions, tb_str_length(definitions), NULL, NULL),
                         ==, TB_INPUT_FILE_OK);
    return input;
}

static tb_input_file_result load(tb_input_player *player, const char *text,
                                 tb_input_file_error *error)
{
    return tb_input_load_bindings(player, text, tb_str_length(text), "bindings.json", error);
}

/* A player's rebinds as a file; the caller frees it. */
static char *save(const tb_input_player *player)
{
    tb_stream stream = {0};
    size_t length = 0;

    tb_stream_open_buffer(NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_input_save_bindings(player, &stream, NULL), ==, TB_INPUT_FILE_OK);
    const unsigned char *const bytes = tb_stream_buffer_bytes(&stream, &length);
    char *const text = tb_malloc(length + 1);
    memcpy(text, bytes, length);
    text[length] = '\0';
    (void)tb_stream_close(&stream);
    return text;
}

/* Only the differences are written, in the order of contexts and mappings,
 * an unbound mapping as null; read into another input, they are the same
 * rebinds, and write the same bytes. */
TB_TEST(input_bindings, rebinds_round_trip_as_differences)
{
    tb_input *const first = make_input();
    tb_input *const second = make_input();
    tb_input_player *const player = tb_input_player_at(first, 0);
    tb_input_player *const loaded = tb_input_player_at(second, 0);
    const tb_input_context gameplay = tb_input_find_context(first, "gameplay");
    const tb_input_context menu = tb_input_find_context(first, "menu");
    tb_input_file_error error = {0};

    tb_input_rebind(player, menu, 0, TB_INPUT_KEY_E);
    tb_input_rebind(player, gameplay, 3, TB_INPUT_KEY_NONE);
    tb_input_rebind(player, gameplay, 0, TB_INPUT_KEY_F);
    char *const written = save(player);
    TB_TEST_EXPECT_STRING(written, ==,
                          "{\n"
                          "  \"format\": \"badger-input-bindings\",\n"
                          "  \"version\": 1,\n"
                          "  \"bindings\": [\n"
                          "    {\n"
                          "      \"context\": \"gameplay\",\n"
                          "      \"action\": \"jump\",\n"
                          "      \"default\": \"space\",\n"
                          "      \"key\": \"f\"\n"
                          "    },\n"
                          "    {\n"
                          "      \"context\": \"gameplay\",\n"
                          "      \"action\": \"fire\",\n"
                          "      \"default\": \"mouse_left\",\n"
                          "      \"slot\": 1,\n"
                          "      \"key\": null\n"
                          "    },\n"
                          "    {\n"
                          "      \"context\": \"menu\",\n"
                          "      \"action\": \"jump\",\n"
                          "      \"default\": \"enter\",\n"
                          "      \"key\": \"e\"\n"
                          "    }\n"
                          "  ]\n"
                          "}\n");
    TB_TEST_REQUIRE_UINT(load(loaded, written, &error), ==, TB_INPUT_FILE_OK);
    TB_TEST_EXPECT_UINT(tb_input_rebind_count(loaded), ==, 3);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(loaded, gameplay, 0), ==, TB_INPUT_KEY_F);
    /* Of two mappings of fire from the left button, the second's rebind,
     * of slot 1, goes to the second. */
    TB_TEST_EXPECT_UINT(tb_input_binding_key(loaded, gameplay, 2), ==, TB_INPUT_KEY_MOUSE_LEFT);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(loaded, gameplay, 3), ==, TB_INPUT_KEY_NONE);
    char *const again = save(loaded);
    TB_TEST_EXPECT_STRING(again, ==, written);
    tb_free(written);
    tb_free(again);
    tb_input_destroy(first);
    tb_input_destroy(second);
}

/* A file replaces every rebind the player had; one of a context, an action
 * or a default the input no longer has is passed over; a rebind to the
 * default is none. */
TB_TEST(input_bindings, loading_replaces_and_passes_over_what_is_gone)
{
    tb_input *const input = make_input();
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_context gameplay = tb_input_find_context(input, "gameplay");
    tb_input_file_error error = {0};

    tb_input_rebind(player, gameplay, 1, TB_INPUT_KEY_GAMEPAD_FACE_EAST);
    TB_TEST_REQUIRE_UINT(
        load(player,
             "{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": [\n"
             " {\"context\": \"vehicle\", \"action\": \"jump\", \"default\": \"space\", \"key\": "
             "\"f\"},\n"
             " {\"context\": \"gameplay\", \"action\": \"crouch\", \"default\": \"space\", "
             "\"key\": \"f\"},\n"
             " {\"context\": \"gameplay\", \"action\": \"jump\", \"default\": \"tab\", \"key\": "
             "\"f\"},\n"
             " {\"context\": \"gameplay\", \"action\": \"jump\", \"default\": \"space\", "
             "\"slot\": 1, \"key\": \"f\"},\n"
             " {\"context\": \"gameplay\", \"action\": \"fire\", \"default\": \"mouse_left\", "
             "\"key\": \"mouse_left\"},\n"
             " {\"context\": \"gameplay\", \"action\": \"jump\", \"default\": \"space\", \"key\": "
             "\"g\"}]}",
             &error),
        ==, TB_INPUT_FILE_OK);
    TB_TEST_EXPECT_UINT(tb_input_rebind_count(player), ==, 1);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(player, gameplay, 0), ==, TB_INPUT_KEY_G);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(player, gameplay, 1), ==,
                        TB_INPUT_KEY_GAMEPAD_FACE_SOUTH);

    /* A file of no bindings is the defaults. */
    TB_TEST_REQUIRE_UINT(
        load(player, "{\"format\": \"badger-input-bindings\", \"version\": 1}", NULL), ==,
        TB_INPUT_FILE_OK);
    TB_TEST_EXPECT_UINT(tb_input_rebind_count(player), ==, 0);
    tb_input_destroy(input);
}

/* Each way a file can be JSON but not bindings is refused, where it is,
 * and changes nothing. */
TB_TEST(input_bindings, broken_files_are_refused_and_change_nothing)
{
    typedef struct broken_case
    {
        const char *text;
        const char *path;
        const char *reason;
    } broken_case;
    static const broken_case cases[] = {
        {"[]", "$", "bindings: an object"},
        {"{\"format\": \"badger-input-bindings\\u0000other\", \"version\": 1}", "$.format",
         "a format other than \"badger-input-bindings\""},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\\u0000x\": []}",
         "$.bindings", "a key the format does not have"},
        {"{\"format\": \"badger-input\", \"version\": 1}", "$.format",
         "a format other than \"badger-input-bindings\""},
        {"{\"version\": 1}", "$.format", "a format other than \"badger-input-bindings\""},
        {"{\"format\": \"badger-input-bindings\"}", "$.version",
         "a version: a whole number from 1"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 0}", "$.version",
         "a version: a whole number from 1"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 2}", "$.version",
         "a newer version than this engine reads"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"extra\": 1}", "$.extra",
         "a key the format does not have"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": {}}", "$.bindings",
         "an array"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": [1]}",
         "$.bindings[0]", "a binding: an object"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": [{\"context\": "
         "\"gameplay\", \"action\": \"jump\", \"default\": \"space\", \"key\": \"f\", \"slots\": "
         "2}]}",
         "$.bindings[0].slots", "a key the format does not have"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": [{\"context\": "
         "\"gameplay\", \"action\": \"jump\", \"default\": \"space\", \"slot\": -1, \"key\": "
         "\"f\"}]}",
         "$.bindings[0].slot", "a slot: a whole number from 0"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": [{\"action\": "
         "\"jump\", \"default\": \"space\", \"key\": \"f\"}]}",
         "$.bindings[0].context", "a string"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": [{\"context\": "
         "\"gameplay\", \"action\": 3, \"default\": \"space\", \"key\": \"f\"}]}",
         "$.bindings[0].action", "a string"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": [{\"context\": "
         "\"gameplay\", \"action\": \"jump\", \"default\": null, \"key\": \"f\"}]}",
         "$.bindings[0].default", "a string"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": [{\"context\": "
         "\"gameplay\", \"action\": \"jump\", \"default\": \"spcae\", \"key\": \"f\"}]}",
         "$.bindings[0].default", "an unknown key"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": [{\"context\": "
         "\"gameplay\", \"action\": \"jump\", \"default\": \"space\"}]}",
         "$.bindings[0].key", "a string"},
        {"{\"format\": \"badger-input-bindings\", \"version\": 1, \"bindings\": [\n"
         "{\"context\": \"gameplay\", \"action\": \"jump\", \"default\": \"space\", \"key\": "
         "\"f\"},\n"
         "{\"context\": \"gameplay\", \"action\": \"jump\", \"default\": \"space\", \"key\": "
         "\"nope\"}]}",
         "$.bindings[1].key", "an unknown key"},
    };
    tb_input *const input = make_input();
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_context menu = tb_input_find_context(input, "menu");
    tb_input_file_error error = {0};

    tb_input_rebind(player, menu, 0, TB_INPUT_KEY_E);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        TB_TEST_CONTEXT("case %zu: %s", index, cases[index].text);
        TB_TEST_EXPECT_UINT(load(player, cases[index].text, &error), ==, TB_INPUT_FILE_INVALID);
        TB_TEST_EXPECT_UINT(error.result, ==, TB_INPUT_FILE_INVALID);
        TB_TEST_EXPECT_STRING(error.path, ==, cases[index].path);
        TB_TEST_EXPECT_STRING(error.reason, ==, cases[index].reason);
        TB_TEST_EXPECT(tb_str_starts_with(error.message, "bindings.json:"));
        TB_TEST_EXPECT_UINT(tb_input_rebind_count(player), ==, 1);
        TB_TEST_EXPECT_UINT(tb_input_binding_key(player, menu, 0), ==, TB_INPUT_KEY_E);
    }
    /* The second binding's key, on line 3. */
    TB_TEST_EXPECT_UINT(error.line, ==, 3);
    TB_TEST_EXPECT_UINT(load(player, "{\"format\": ", &error), ==, TB_INPUT_FILE_MALFORMED);
    TB_TEST_EXPECT_UINT(error.line, ==, 1);
    TB_TEST_EXPECT_UINT(load(player, "nope", NULL), ==, TB_INPUT_FILE_MALFORMED);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(player, menu, 0), ==, TB_INPUT_KEY_E);
    tb_input_destroy(input);
}

/* ---------------------------------------------------------------------------
 * Through a VFS, and a stream that refuses.
 * ------------------------------------------------------------------------- */

typedef struct memory_file
{
    const char *bytes;
    size_t length;
    bool missing;
} memory_file;

static tb_vfs_result memory_open(void *state, const char *name, size_t length,
                                 const tb_allocator *allocator, tb_stream *stream)
{
    memory_file *const file = state;

    if (file->missing || length != tb_str_length(BINDINGS_PATH) ||
        memcmp(name, BINDINGS_PATH, length) != 0)
    {
        return TB_VFS_NOT_FOUND;
    }
    tb_stream_open_memory(file->bytes, file->length, allocator, stream);
    return TB_VFS_OK;
}

static tb_vfs_result memory_query(void *state, const char *name, size_t length, tb_vfs_info *info)
{
    const memory_file *const file = state;

    (void)name;
    (void)length;
    if (file->missing)
    {
        return TB_VFS_NOT_FOUND;
    }
    *info = (tb_vfs_info){.kind = TB_VFS_FILE, .size = file->length};
    return TB_VFS_OK;
}

static tb_vfs_result memory_list(void *state, const char *directory, size_t length,
                                 tb_vfs_list_function *function, void *context)
{
    (void)state;
    (void)directory;
    (void)length;
    (void)function;
    (void)context;
    return TB_VFS_NOT_FOUND;
}

static void memory_release(void *state)
{
    (void)state;
}

static const tb_vfs_source_functions memory_functions = {memory_open, memory_query, memory_list,
                                                         memory_release};

TB_TEST(input_bindings, files_load_through_a_vfs)
{
    static const char text[] = "{\"format\": \"badger-input-bindings\", \"version\": 1, "
                               "\"bindings\": [{\"context\": \"menu\", \"action\": \"jump\", "
                               "\"default\": \"enter\", \"key\": \"tab\"}]}";
    memory_file file = {.bytes = text, .length = sizeof(text) - 1};
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_input *const input = make_input();
    tb_input_player *const player = tb_input_player_at(input, 0);
    tb_input_file_error error = {0};

    TB_TEST_REQUIRE_UINT(tb_vfs_mount(vfs, "", 0, (tb_vfs_source){&memory_functions, &file}, 0), ==,
                         TB_VFS_OK);
    TB_TEST_REQUIRE_UINT(tb_input_load_bindings_file(player, vfs, BINDINGS_PATH, &error), ==,
                         TB_INPUT_FILE_OK);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(player, tb_input_find_context(input, "menu"), 0), ==,
                        TB_INPUT_KEY_TAB);
    file.missing = true;
    TB_TEST_EXPECT_UINT(tb_input_load_bindings_file(player, vfs, BINDINGS_PATH, &error), ==,
                        TB_INPUT_FILE_NOT_FOUND);
    TB_TEST_EXPECT_STRING(error.message, ==, BINDINGS_PATH ": no such file");
    TB_TEST_EXPECT_UINT(tb_input_load_bindings_file(player, vfs, BINDINGS_PATH, NULL), ==,
                        TB_INPUT_FILE_NOT_FOUND);
    tb_input_destroy(input);
    tb_vfs_destroy(vfs);
}

static tb_stream_result refusing_write(void *state, const void *bytes, size_t length,
                                       size_t *transferred)
{
    (void)state;
    (void)bytes;
    (void)length;
    *transferred = 0;
    return TB_STREAM_NO_SPACE;
}

static tb_stream_result nothing_to_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

TB_TEST(input_bindings, saving_fails_when_the_stream_refuses)
{
    static const tb_stream_functions refusing_functions = {.write_bytes = refusing_write,
                                                           .release = nothing_to_release};
    tb_stream refusing = {&refusing_functions, NULL};
    tb_input *const input = make_input();
    tb_input_file_error error = {0};

    TB_TEST_EXPECT_UINT(tb_input_save_bindings(tb_input_player_at(input, 0), &refusing, &error), ==,
                        TB_INPUT_FILE_OUTPUT_FAILED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the stream refused what was written");
    TB_TEST_EXPECT_UINT(tb_input_save_bindings(tb_input_player_at(input, 0), &refusing, NULL), ==,
                        TB_INPUT_FILE_OUTPUT_FAILED);
    tb_input_destroy(input);
}
