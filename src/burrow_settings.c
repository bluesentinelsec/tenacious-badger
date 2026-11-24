/* Burrow's settings, loaded layer by layer: the engine's built-in values,
 * Burrow's defaults, the player's file, then the command line. */
#include "burrow_settings.h"

#include "badger/cli.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/str.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* The directory of the player's file, in the user's configuration directory. */
#define BURROW_APPLICATION "Burrow"

/* Bytes enough for any value `burrow settings` lists. */
#define BURROW_VALUE_CAPACITY (TB_SETTING_DEFAULT_TEXT_LIMIT * 2 + 3)

/* Bytes enough for any int64_t in decimal. */
#define BURROW_FRAMES_CAPACITY 24

/* Burrow's defaults, burrow_defaults.cfg, which the build embeds, and the
 * name its problems are reported under. */
extern const char burrow_defaults[];
extern const size_t burrow_defaults_length;
static char burrow_defaults_name[] = "burrow_defaults.cfg";

static tb_path_view burrow_view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}

/* Writes a problem with a file to standard error, naming the file. */
static void burrow_report(void *context, const tb_settings_problem *problem)
{
    const char *const source = context;

    (void)tb_fmt_print_error("burrow: %s: %s\n", source, problem->message);
}

/* The player's file: the one the command line names, or the usual one. */
static int burrow_find_player_file(const burrow_settings_request *request, char **path)
{
    tb_fs_result result = TB_FS_OK;

    if (request->player_file != NULL)
    {
        *path = tb_str_dup(NULL, request->player_file);
        return EXIT_SUCCESS;
    }
    result = tb_settings_player_file_path(BURROW_APPLICATION, NULL, path);
    /* coverage-off: only a system without a configuration directory fails */
    if (result != TB_FS_OK)
    {
        (void)tb_fmt_print_error("burrow: error: no place for the settings file: %s\n",
                                 tb_fs_result_name(result));
        return EXIT_FAILURE;
    }
    /* coverage-on */
    return EXIT_SUCCESS;
}

static int burrow_load_player_file(const burrow_settings_request *request, burrow_settings *loaded)
{
    const tb_path_view path = burrow_view(loaded->player_path);
    tb_fs_result result = TB_FS_OK;

    if (request->reset)
    {
        result = tb_settings_set_aside_file(path);
        if (result != TB_FS_OK)
        {
            (void)tb_fmt_print_error("burrow: error: %s could not be set aside: %s\n",
                                     loaded->player_path, tb_fs_result_name(result));
            return EXIT_FAILURE;
        }
        (void)tb_fmt_print_error("burrow: settings reset to the defaults; the old ones, if "
                                 "any, are in %s.bak\n",
                                 loaded->player_path);
        return EXIT_SUCCESS;
    }
    result = tb_settings_load_file(loaded->settings, TB_SETTING_LAYER_PLAYER_FILE, path,
                                   burrow_report, loaded->player_path);
    /* A player who never saved has no file; any other failure leaves the
     * defaults, which is how the game starts, said once. */
    if (result != TB_FS_OK && result != TB_FS_NOT_FOUND)
    {
        (void)tb_fmt_print_error("burrow: %s could not be read: %s; starting from the defaults\n",
                                 loaded->player_path, tb_fs_result_name(result));
    }
    return EXIT_SUCCESS;
}

/* Each NAME=VALUE of the command line, at its layer. */
static int burrow_apply_values(const burrow_settings_request *request, burrow_settings *loaded)
{
    for (size_t index = 0; index < request->value_count; index++)
    {
        const char *const assignment = request->values[index];
        const size_t equals = tb_str_find_char(assignment, 0, TB_STR_END, '=');
        char *name = NULL;
        tb_setting_result result = TB_SETTING_OK;

        if (equals == TB_STR_NOT_FOUND)
        {
            (void)tb_fmt_print_error("burrow: error: --set %s: expected NAME=VALUE\n", assignment);
            return TB_CLI_EXIT_USAGE;
        }
        name = tb_str_sub(NULL, assignment, 0, equals);
        result = tb_settings_set(loaded->settings, TB_SETTING_LAYER_COMMAND_LINE, name,
                                 assignment + equals + 1);
        if (result != TB_SETTING_OK)
        {
            (void)tb_fmt_print_error("burrow: error: --set %s: %s\n", assignment,
                                     result == TB_SETTING_UNKNOWN
                                         ? "no setting has the name"
                                         : "the value does not fit the setting");
            tb_str_free(NULL, name);
            return TB_CLI_EXIT_USAGE;
        }
        tb_str_free(NULL, name);
    }
    return EXIT_SUCCESS;
}

/* --renderer=NAME, a shorthand for --set graphics.renderer=NAME that names
 * the renderers when it is wrong. */
static int burrow_apply_renderer(const burrow_settings_request *request, burrow_settings *loaded)
{
    const tb_setting *const renderer = loaded->engine.graphics.renderer;

    if (request->renderer == NULL)
    {
        return EXIT_SUCCESS;
    }
    if (tb_settings_set(loaded->settings, TB_SETTING_LAYER_COMMAND_LINE, "graphics.renderer",
                        request->renderer) == TB_SETTING_OK)
    {
        return EXIT_SUCCESS;
    }
    (void)tb_fmt_print_error("burrow: error: --renderer=%s: not a renderer; the renderers are ",
                             request->renderer);
    for (size_t index = 0; index < tb_setting_choice_count(renderer); index++)
    {
        const char *const separator = index == 0 ? "" : ", ";
        (void)tb_fmt_print_error("%s%s", separator, tb_setting_choice_name(renderer, index));
    }
    (void)tb_fmt_print_error("\n");
    return TB_CLI_EXIT_USAGE;
}

/* --frames=N, a shorthand for --set developer.frame_count=N: the escape
 * hatch that runs the game for a number of frames, for tests. */
static void burrow_apply_frames(const burrow_settings_request *request, burrow_settings *loaded)
{
    char value[BURROW_FRAMES_CAPACITY] = {0};

    if (request->frames <= 0)
    {
        return;
    }
    (void)tb_fmt_buffer(value, sizeof(value), "%" PRId64, request->frames);
    (void)tb_settings_set(loaded->settings, TB_SETTING_LAYER_COMMAND_LINE, "developer.frame_count",
                          value);
}

int burrow_settings_load(const burrow_settings_request *request, burrow_settings *loaded)
{
    const tb_engine_settings_options options = {.developer = request->developer};
    int status = EXIT_SUCCESS;

    *loaded = (burrow_settings){0};
    loaded->settings = tb_settings_create();
    tb_engine_settings_add(loaded->settings, &options, &loaded->engine);
    (void)tb_settings_load_text(loaded->settings, TB_SETTING_LAYER_DEFAULT_FILE, burrow_defaults,
                                burrow_defaults_length, burrow_report, burrow_defaults_name);
    status = burrow_find_player_file(request, &loaded->player_path);
    if (status == EXIT_SUCCESS)
    {
        status = burrow_load_player_file(request, loaded);
    }
    if (status == EXIT_SUCCESS)
    {
        status = burrow_apply_values(request, loaded);
    }
    if (status == EXIT_SUCCESS)
    {
        status = burrow_apply_renderer(request, loaded);
    }
    if (status == EXIT_SUCCESS)
    {
        burrow_apply_frames(request, loaded);
    }
    if (status != EXIT_SUCCESS)
    {
        burrow_settings_release(loaded);
    }
    return status;
}

void burrow_settings_release(burrow_settings *loaded)
{
    tb_str_free(NULL, loaded->player_path);
    tb_settings_destroy(loaded->settings);
    *loaded = (burrow_settings){0};
}

void burrow_settings_list(const burrow_settings *loaded)
{
    const size_t families = tb_settings_family_count(loaded->settings);
    const size_t count = tb_settings_count(loaded->settings);

    (void)tb_fmt_print("# The player's settings file: %s\n", loaded->player_path);
    for (size_t family = 0; family < families; family++)
    {
        const tb_settings_family about = tb_settings_family_at(loaded->settings, family);
        (void)tb_fmt_print("\n# %s%s\n", about.title, about.for_players ? "" : " (not saved)");
        for (size_t index = 0; index < count; index++)
        {
            const tb_setting *const setting = tb_settings_at(loaded->settings, index);
            char value[BURROW_VALUE_CAPACITY] = {0};
            if (tb_setting_family(setting) != family)
            {
                continue;
            }
            (void)tb_setting_format(setting, TB_SETTING_LAYER_COUNT, value, sizeof(value));
            (void)tb_fmt_print("%s = %s  # %s\n", tb_setting_name(setting), value,
                               tb_setting_layer_name(tb_setting_source(setting)));
        }
    }
}
