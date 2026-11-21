/* Settings text and files: loading a layer, writing the player's choices,
 * and where the player's file lives. */
#include "settings_internal.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/memory.h"
#include "badger/sort.h"
#include "badger/str.h"
#include "badger/text.h"

#include <string.h>

/* The name of the player's file, in its game's directory. */
#define TB_SETTINGS_FILE_NAME "settings.cfg"

/* What is added to a file's name to set it aside. */
#define TB_SETTINGS_ASIDE_SUFFIX ".bak"

/* The first line of a file the registry writes. */
#define TB_SETTINGS_HEADER                                                                         \
    "# Settings, one a line as name = value. Lines that start with # are comments.\n"

/* Introduces the entries of a player's file that no setting of this version
 * of the game has. */
#define TB_SETTINGS_KEPT_HEADER "\n# Settings that this version of the game does not have\n"

/* ---------------------------------------------------------------------------
 * Reading text.
 * ------------------------------------------------------------------------- */

/* A line of the text that sets a setting: its name and its unquoted value,
 * both the heap's. */
typedef struct tb_settings_line
{
    size_t number;
    char *name;
    char *value;
    size_t value_length;
} tb_settings_line;

/* A value a layer will hold once the text is loaded. */
typedef struct tb_settings_pending
{
    tb_setting *setting;
    tb_setting_value value;
} tb_settings_pending;

typedef struct tb_settings_loader
{
    tb_setting_layer layer;
    tb_array *lines;    /* tb_settings_line. */
    tb_array *problems; /* tb_settings_problem. */
    tb_array *pending;  /* tb_settings_pending. */
} tb_settings_loader;

static bool tb_settings_blank(char character)
{
    return character == ' ' || character == '\t';
}

static void tb_settings_trim(const char **text, size_t *length)
{
    while (*length > 0 && tb_settings_blank((*text)[0]))
    {
        (*text)++;
        (*length)--;
    }
    while (*length > 0 && tb_settings_blank((*text)[*length - 1]))
    {
        (*length)--;
    }
}

static void tb_settings_problem_at(tb_settings_loader *loader, size_t line,
                                   tb_setting_result result, const char *message)
{
    tb_settings_problem problem = {.layer = loader->layer, .line = line, .result = result};

    (void)tb_fmt_buffer(problem.message, sizeof(problem.message), "line %zu: %s", line, message);
    (void)tb_array_push(loader->problems, &problem);
}

/* Read one line, which is neither blank nor a comment, into the loader. */
static void tb_settings_read_line(tb_settings_loader *loader, size_t number, const char *text,
                                  size_t length)
{
    const size_t equals = tb_text_find_char(tb_text_make(text, length), 0, length, '=');
    const char *name = text;
    size_t name_length = equals;
    const char *value = NULL;
    size_t value_length = 0;
    const char *problem = NULL;
    char message[TB_SETTINGS_MESSAGE_CAPACITY] = {0};
    tb_settings_line line = {.number = number};

    if (equals == TB_TEXT_NOT_FOUND || equals >= length)
    {
        tb_settings_problem_at(loader, number, TB_SETTING_INVALID, "expected 'name = value'");
        return;
    }
    value = text + equals + 1;
    value_length = length - equals - 1;
    tb_settings_trim(&name, &name_length);
    tb_settings_trim(&value, &value_length);
    if (!tb_settings_valid_name(name, name_length, 2, 0))
    {
        (void)tb_fmt_buffer(message, sizeof(message), "'%.*s' is not a setting's name",
                            (int)(name_length < 64 ? name_length : 64), name);
        tb_settings_problem_at(loader, number, TB_SETTING_INVALID, message);
        return;
    }
    line.value = tb_settings_unquote(value, value_length, &line.value_length, &problem);
    if (line.value == NULL)
    {
        (void)tb_fmt_buffer(message, sizeof(message), "'%.*s': %s", (int)name_length, name,
                            problem);
        tb_settings_problem_at(loader, number, TB_SETTING_INVALID, message);
        return;
    }
    line.name = tb_malloc(name_length + 1);
    memcpy(line.name, name, name_length);
    line.name[name_length] = '\0';
    (void)tb_array_push(loader->lines, &line);
}

/* Split the text into lines and read those that are not blank or comments. */
static void tb_settings_read_lines(tb_settings_loader *loader, const char *text, size_t length)
{
    static const char byte_order_mark[] = "\xEF\xBB\xBF";
    size_t start = 0;
    size_t number = 1;

    if (length >= 3 && memcmp(text, byte_order_mark, 3) == 0)
    {
        start = 3;
    }
    while (start < length)
    {
        size_t end = tb_text_find_char(tb_text_make(text, length), start, length, '\n');
        const size_t next = end == TB_TEXT_NOT_FOUND ? length : end + 1;
        const char *line = text + start;
        size_t line_length = 0;

        if (end == TB_TEXT_NOT_FOUND)
        {
            end = length;
        }
        line_length = end - start;
        if (line_length > 0 && line[line_length - 1] == '\r')
        {
            line_length--;
        }
        tb_settings_trim(&line, &line_length);
        if (line_length > 0 && line[0] != '#')
        {
            tb_settings_read_line(loader, number, line, line_length);
        }
        start = next;
        number++;
    }
}

/* Keep a line for a name no setting has, replacing what the layer kept for
 * the name before. Takes the line's strings. */
static void tb_settings_keep(tb_settings *settings, tb_setting_layer layer, tb_settings_line *line)
{
    tb_settings_kept kept = {.layer = layer, .name = line->name, .value = line->value};

    for (size_t index = 0; index < tb_array_length(settings->kept); index++)
    {
        tb_settings_kept *const old = TB_ARRAY_AT(settings->kept, tb_settings_kept, index);
        if (old->layer == layer && tb_str_equal(old->name, kept.name))
        {
            tb_free(old->name);
            tb_free(old->value);
            *old = kept;
            line->name = NULL;
            line->value = NULL;
            return;
        }
    }
    (void)tb_array_push(settings->kept, &kept);
    line->name = NULL;
    line->value = NULL;
}

static void tb_settings_forget_kept(tb_settings *settings, tb_setting_layer layer)
{
    size_t index = 0;

    while (index < tb_array_length(settings->kept))
    {
        tb_settings_kept *const kept = TB_ARRAY_AT(settings->kept, tb_settings_kept, index);
        if (kept->layer != layer)
        {
            index++;
            continue;
        }
        tb_free(kept->name);
        tb_free(kept->value);
        tb_array_remove(settings->kept, index, NULL);
    }
}

/* The value the text gives a setting, replacing one an earlier line gave. */
static void tb_settings_note_pending(tb_settings_loader *loader, tb_setting *setting,
                                     tb_setting_value value)
{
    tb_settings_pending entry = {.setting = setting, .value = value};

    for (size_t index = 0; index < tb_array_length(loader->pending); index++)
    {
        tb_settings_pending *const old = TB_ARRAY_AT(loader->pending, tb_settings_pending, index);
        if (old->setting == setting)
        {
            if (setting->type == TB_SETTING_TEXT)
            {
                tb_free(old->value.text);
            }
            old->value = value;
            return;
        }
    }
    (void)tb_array_push(loader->pending, &entry);
}

/* Resolve every line to its setting, with the registry locked. */
static void tb_settings_resolve_lines(tb_settings *settings, tb_settings_loader *loader)
{
    for (size_t index = 0; index < tb_array_length(loader->lines); index++)
    {
        tb_settings_line *const line = TB_ARRAY_AT(loader->lines, tb_settings_line, index);
        tb_setting *const setting = tb_table_get(settings->by_name, line->name);
        tb_setting_value value = {0};
        tb_setting_result result = TB_SETTING_OK;
        char message[TB_SETTINGS_MESSAGE_CAPACITY] = {0};

        if (setting == NULL)
        {
            (void)tb_fmt_buffer(message, sizeof(message), "'%s' is not a setting", line->name);
            tb_settings_problem_at(loader, line->number, TB_SETTING_UNKNOWN, message);
            tb_settings_keep(settings, loader->layer, line);
            continue;
        }
        result = tb_setting_read_value(setting, line->value, line->value_length, &value);
        if (result != TB_SETTING_OK)
        {
            tb_setting_describe_failure(setting, result, line->value, line->value_length, message,
                                        sizeof(message));
            tb_settings_problem_at(loader, line->number, result, message);
            continue;
        }
        tb_settings_note_pending(loader, setting, value);
    }
}

/* Give the layer of every setting its new value, or none, so that each
 * setting's version moves once at most. */
static void tb_settings_apply_pending(tb_settings *settings, tb_settings_loader *loader)
{
    for (size_t index = 0; index < tb_array_length(settings->settings); index++)
    {
        tb_setting *const setting = *TB_ARRAY_AT(settings->settings, tb_setting *, index);
        bool found = false;

        for (size_t entry = 0; entry < tb_array_length(loader->pending); entry++)
        {
            const tb_settings_pending *const pending =
                TB_ARRAY_AT(loader->pending, tb_settings_pending, entry);
            if (pending->setting == setting)
            {
                tb_setting_store_locked(setting, loader->layer, pending->value);
                found = true;
                break;
            }
        }
        if (!found)
        {
            tb_setting_clear_locked(setting, loader->layer);
        }
    }
}

/* Problems in the order of their lines. Lines that are not `name = value`
 * are found before the others, while the text is read. */
static int tb_settings_compare_problems(const void *left, const void *right, void *context)
{
    const tb_settings_problem *const first = left;
    const tb_settings_problem *const second = right;

    (void)context;
    if (first->line != second->line)
    {
        return first->line < second->line ? -1 : 1;
    }
    return 0;
}

static void tb_settings_release_loader(tb_settings_loader *loader)
{
    for (size_t index = 0; index < tb_array_length(loader->lines); index++)
    {
        tb_settings_line *const line = TB_ARRAY_AT(loader->lines, tb_settings_line, index);
        tb_free(line->name);
        tb_free(line->value);
    }
    tb_array_destroy(loader->lines);
    tb_array_destroy(loader->problems);
    tb_array_destroy(loader->pending);
}

size_t tb_settings_load_text(tb_settings *settings, tb_setting_layer layer, const char *text,
                             size_t length, tb_settings_report_function *report, void *context)
{
    tb_settings_loader loader = {.layer = layer};
    size_t problems = 0;

    TB_ASSERT(settings != NULL);
    TB_ASSERT_MSG(layer > TB_SETTING_LAYER_BUILT_IN && layer < TB_SETTING_LAYER_COUNT,
                  "the layer is one a value may be set at");
    TB_ASSERT(text != NULL || length == 0);
    loader.lines = TB_ARRAY_CREATE(NULL, tb_settings_line);
    loader.problems = TB_ARRAY_CREATE(NULL, tb_settings_problem);
    loader.pending = TB_ARRAY_CREATE(NULL, tb_settings_pending);
    if (length > 0)
    {
        tb_settings_read_lines(&loader, text, length);
    }

    tb_mutex_lock(settings->mutex);
    tb_settings_forget_kept(settings, layer);
    tb_settings_resolve_lines(settings, &loader);
    tb_settings_apply_pending(settings, &loader);
    tb_mutex_unlock(settings->mutex);

    problems = tb_array_length(loader.problems);
    tb_sort_stable(tb_array_data(loader.problems), problems, sizeof(tb_settings_problem),
                   tb_settings_compare_problems, NULL, NULL);
    for (size_t index = 0; index < problems && report != NULL; index++)
    {
        report(context, TB_ARRAY_AT(loader.problems, tb_settings_problem, index));
    }
    tb_settings_release_loader(&loader);
    return problems;
}

tb_fs_result tb_settings_load_file(tb_settings *settings, tb_setting_layer layer, tb_path_view path,
                                   tb_settings_report_function *report, void *context)
{
    tb_fs_buffer buffer = {0};
    tb_fs_result result = TB_FS_OK;

    TB_ASSERT(settings != NULL);
    result = tb_fs_read_file(path, NULL, TB_SETTINGS_FILE_LIMIT, &buffer);
    if (result != TB_FS_OK)
    {
        return result;
    }
    (void)tb_settings_load_text(settings, layer, (const char *)buffer.data, buffer.length, report,
                                context);
    tb_fs_buffer_free(&buffer);
    return TB_FS_OK;
}

/* ---------------------------------------------------------------------------
 * Writing text.
 * ------------------------------------------------------------------------- */

static void tb_settings_append(tb_array *text, const char *bytes, size_t length)
{
    tb_array_append(text, bytes, length);
}

static void tb_settings_append_string(tb_array *text, const char *string)
{
    tb_settings_append(text, string, tb_str_length(string));
}

static void tb_settings_append_value(tb_array *text, const tb_setting *setting,
                                     const tb_setting_value *value)
{
    const size_t start = tb_array_length(text);
    const size_t length = tb_setting_write_value(setting, value, NULL, 0);

    tb_array_resize(text, start + length + 1);
    (void)tb_setting_write_value(setting, value, TB_ARRAY_AT(text, char, start), length + 1);
    tb_array_resize(text, start + length);
}

/* The value a player chose for a setting: the runtime layer's, or the player
 * file's; NULL when neither has one. */
static const tb_setting_value *tb_settings_player_choice(const tb_setting *setting)
{
    if ((setting->present & (1u << TB_SETTING_LAYER_RUNTIME)) != 0)
    {
        return &setting->values[TB_SETTING_LAYER_RUNTIME];
    }
    if ((setting->present & (1u << TB_SETTING_LAYER_PLAYER_FILE)) != 0)
    {
        return &setting->values[TB_SETTING_LAYER_PLAYER_FILE];
    }
    return NULL;
}

static void tb_settings_write_family(const tb_settings *settings, size_t family, tb_array *text)
{
    const tb_settings_family_record *const record =
        TB_ARRAY_AT(settings->families, tb_settings_family_record, family);
    bool started = false;

    if (!record->for_players)
    {
        return;
    }
    for (size_t index = 0; index < tb_array_length(settings->settings); index++)
    {
        const tb_setting *const setting = *TB_ARRAY_AT(settings->settings, tb_setting *, index);
        const tb_setting_value *const value = tb_settings_player_choice(setting);
        if (setting->family != family || value == NULL)
        {
            continue;
        }
        if (!started)
        {
            tb_settings_append_string(text, "\n# ");
            tb_settings_append_string(text, record->title);
            tb_settings_append_string(text, "\n");
            started = true;
        }
        tb_settings_append_string(text, setting->name);
        tb_settings_append_string(text, " = ");
        tb_settings_append_value(text, setting, value);
        tb_settings_append_string(text, "\n");
    }
}

/* A kept value, quoted, since nothing says what type it is. */
static void tb_settings_write_kept(const tb_settings_kept *kept, tb_array *text)
{
    static const tb_setting quoting = {.type = TB_SETTING_TEXT};
    const tb_setting_value value = {.text = kept->value};

    tb_settings_append_string(text, kept->name);
    tb_settings_append_string(text, " = ");
    tb_settings_append_value(text, &quoting, &value);
    tb_settings_append_string(text, "\n");
}

char *tb_settings_write_text(const tb_settings *settings, const tb_allocator *allocator,
                             size_t *length)
{
    tb_array *const text = TB_ARRAY_CREATE(NULL, char);
    bool kept_started = false;
    char *result = NULL;

    TB_ASSERT(settings != NULL);
    TB_ASSERT(length != NULL);
    tb_settings_append_string(text, TB_SETTINGS_HEADER);
    tb_mutex_lock(settings->mutex);
    for (size_t family = 0; family < tb_array_length(settings->families); family++)
    {
        tb_settings_write_family(settings, family, text);
    }
    for (size_t index = 0; index < tb_array_length(settings->kept); index++)
    {
        const tb_settings_kept *const kept = TB_ARRAY_AT(settings->kept, tb_settings_kept, index);
        if (kept->layer != TB_SETTING_LAYER_PLAYER_FILE)
        {
            continue;
        }
        if (!kept_started)
        {
            tb_settings_append_string(text, TB_SETTINGS_KEPT_HEADER);
            kept_started = true;
        }
        tb_settings_write_kept(kept, text);
    }
    tb_mutex_unlock(settings->mutex);

    *length = tb_array_length(text);
    result = tb_allocator_alloc(allocator, *length + 1, 1);
    memcpy(result, tb_array_data(text), *length);
    result[*length] = '\0';
    tb_array_destroy(text);
    return result;
}

tb_fs_result tb_settings_save_file(const tb_settings *settings, tb_path_view path)
{
    size_t length = 0;
    char *text = NULL;
    char *directory = NULL;
    tb_path_result path_result = TB_PATH_OK;
    tb_fs_result result = TB_FS_OK;

    TB_ASSERT(settings != NULL);
    directory = tb_path_parent_new(NULL, tb_fs_native_style(), path, &path_result);
    if (directory == NULL)
    {
        return TB_FS_INVALID;
    }
    result = tb_fs_make_directories((tb_path_view){directory, tb_str_length(directory)});
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    text = tb_settings_write_text(settings, NULL, &length);
    result = tb_fs_replace_file(path, text, length);

cleanup:
    tb_str_free(NULL, text);
    tb_str_free(NULL, directory);
    return result;
}

/* ---------------------------------------------------------------------------
 * Where the files live.
 * ------------------------------------------------------------------------- */

/* Whether a name is one path component on every platform: not empty, not a
 * dot or two, valid UTF-8, without separators, a drive's colon, or control
 * characters. */
static bool tb_settings_one_component(const char *name)
{
    const size_t length = tb_str_length(name);

    if (length == 0 || tb_str_equal(name, ".") || tb_str_equal(name, ".."))
    {
        return false;
    }
    if (!tb_settings_valid_utf8(name, length))
    {
        return false;
    }
    for (size_t index = 0; index < length; index++)
    {
        const unsigned char character = (unsigned char)name[index];
        if (character < 0x20 || character == '/' || character == '\\' || character == ':')
        {
            return false;
        }
    }
    return true;
}

tb_fs_result tb_settings_player_file_path(const char *application, const tb_allocator *allocator,
                                          char **path)
{
    const tb_path_style style = tb_fs_native_style();
    size_t required = 0;
    char *base = NULL;
    char *directory = NULL;
    tb_path_result joined = TB_PATH_OK;
    tb_fs_result result = TB_FS_OK;

    TB_ASSERT(application != NULL);
    TB_ASSERT(path != NULL);
    *path = NULL;
    if (!tb_settings_one_component(application))
    {
        return TB_FS_INVALID;
    }
    result = tb_fs_get_location(TB_FS_USER_CONFIG, NULL, 0, &required);
    if (result != TB_FS_OK && result != TB_FS_BUFFER_TOO_SMALL)
    {
        return result;
    }
    base = tb_malloc(required);
    result = tb_fs_get_location(TB_FS_USER_CONFIG, base, required, &required);
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    directory = tb_path_join_new(NULL, style, (tb_path_view){base, tb_str_length(base)},
                                 (tb_path_view){application, tb_str_length(application)}, &joined);
    if (directory == NULL)
    {
        result = TB_FS_INVALID;
        goto cleanup;
    }
    *path = tb_path_join_new(
        allocator, style, (tb_path_view){directory, tb_str_length(directory)},
        (tb_path_view){TB_SETTINGS_FILE_NAME, sizeof(TB_SETTINGS_FILE_NAME) - 1}, &joined);
    result = *path != NULL ? TB_FS_OK : TB_FS_INVALID;

cleanup:
    tb_str_free(NULL, directory);
    tb_free(base);
    return result;
}

tb_fs_result tb_settings_set_aside_file(tb_path_view path)
{
    const size_t suffix = sizeof(TB_SETTINGS_ASIDE_SUFFIX) - 1;
    char *aside = NULL;
    tb_fs_result result = TB_FS_OK;

    TB_ASSERT(path.data != NULL || path.length == 0);
    aside = tb_malloc(path.length + suffix + 1);
    if (path.length > 0)
    {
        memcpy(aside, path.data, path.length);
    }
    memcpy(aside + path.length, TB_SETTINGS_ASIDE_SUFFIX, suffix + 1);
    result = tb_fs_rename(path, (tb_path_view){aside, path.length + suffix});
    tb_free(aside);
    return result == TB_FS_NOT_FOUND ? TB_FS_OK : result;
}
