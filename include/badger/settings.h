/** @file settings.h
 * @brief Settings: the options a player or a developer chooses, with their
 * defaults, changed at any time while the game runs.
 *
 * A setting is a named, typed value: the master volume, the window mode, the
 * mouse sensitivity. Subsystems and games add the settings they need to one
 * registry, read them wherever they are used, and change them whenever the
 * player does: in the options menu of a paused game, from the command line,
 * or from a file.
 *
 * @code
 * tb_settings *const settings = tb_settings_create();
 * tb_settings_add_family(settings, &(tb_settings_family){
 *     .name = "audio", .title = "Audio", .for_players = true});
 * tb_setting *const music = tb_settings_add(settings, &(tb_setting_definition){
 *     .name = "audio.music_volume",
 *     .description = "How loud the music plays, from silent to full.",
 *     .type = TB_SETTING_NUMBER,
 *     .number = {.default_value = 0.8, .minimum = 0.0, .maximum = 1.0}});
 *
 * (void)tb_settings_load_file(settings, TB_SETTING_LAYER_PLAYER_FILE, path, NULL, NULL);
 * mixer_set_music_volume(tb_setting_number(music));
 *
 * // The options menu, while the game is paused:
 * (void)tb_setting_set_number(music, 0.5);
 * (void)tb_settings_save_file(settings, path);
 * @endcode
 *
 * **Names and families.** A name is lowercase words of letters, digits and
 * underscores, joined by dots: `audio.music_volume`,
 * `controls.gamepad.vibration`. The first word names the setting's family,
 * which groups it as an options menu does (Display, Audio, Controls), and
 * which must be added before the setting. A family is either for players,
 * shown in options menus and kept in the player's file, or not: engine
 * tuning and developer switches, which only a default file, the command line
 * or a running game set.
 *
 * **Where a value comes from.** A setting holds a value for each of five
 * layers, and its value is that of the highest layer that has one:
 *
 * | Layer | Set by |
 * | --- | --- |
 * | #TB_SETTING_LAYER_RUNTIME, highest | the game while it runs: the options menu, a console |
 * | #TB_SETTING_LAYER_COMMAND_LINE | the command line the game started with |
 * | #TB_SETTING_LAYER_PLAYER_FILE | the player's settings file |
 * | #TB_SETTING_LAYER_DEFAULT_FILE | the game's own file of defaults |
 * | #TB_SETTING_LAYER_BUILT_IN, lowest | the definition; always present |
 *
 * So the game's defaults override the engine's, the player's choices override
 * the game's, a command line overrides the files for one session, and a
 * change made while the game runs overrides everything. The layers are kept
 * apart: saving writes the player's choices, those of the runtime and player
 * file layers, and never a value the command line gave for one session.
 *
 * **Changing a setting at any time.** Any thread may read or change any
 * setting at any moment. A setting's version grows by one whenever its value
 * changes, and the registry's whenever any value does: a subsystem keeps the
 * version it last applied, compares it at a moment that suits it, such as the
 * start of a frame, and applies the new value then. Nothing has to wait for a
 * restart, and no subsystem is called back in the middle of its work.
 *
 * **Files** hold one setting a line, as `name = value`. Lines that start with
 * `#` are comments, and blank lines are ignored.
 *
 * @code
 * # Display
 * display.window_mode = borderless
 * display.frame_limit = 144
 * language.text = "fr-FR"
 * @endcode
 *
 * A boolean is `true` or `false` (also `on`/`off`, `yes`/`no`, `1`/`0`); an
 * integer is decimal digits with an optional sign; a number is a decimal
 * number; a choice is the name of one of its choices; and text is quoted,
 * with `\"`, `\\`, `\n` and `\t` as escapes, or bare to the end of the line.
 * Any value may be quoted. A file may begin with a UTF-8 byte order mark and
 * may end its lines with CR LF.
 *
 * A line that names a setting nobody has added is reported, as
 * #TB_SETTING_UNKNOWN, and kept: it applies if the setting is added later,
 * and saving writes it back, so a file shared by two versions of a game loses
 * nothing. Every other problem, a line that is not `name = value` or a value
 * that does not fit its setting, is reported and skipped, and the line's
 * setting keeps the value of the layers below.
 *
 * **Restoring defaults.** tb_setting_restore_default(),
 * tb_settings_restore_family() and tb_settings_restore_all() remove the
 * player's choices, as an options menu's "restore defaults" does.
 * tb_settings_set_aside_file() moves a player's file out of the way, for a
 * command line that starts a game whose settings left it unusable.
 *
 * **Errors.** Adding a setting wrongly, with a bad name, an empty range or a
 * family that was never added, is a contract violation, as is reading or
 * writing a setting as a type it is not. Values that do not fit, from the
 * player or a file, are ordinary failures that the functions report.
 *
 * @par Threading
 * Thread-safe: every function but tb_settings_create() and
 * tb_settings_destroy() may be called from any thread at any time. Those two
 * are externally synchronized with everything else.
 */
#ifndef TB_SETTINGS_H
#define TB_SETTINGS_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/file.h"
#include "badger/path.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief A registry of settings. Created by tb_settings_create(); owned by
 * the caller; released by tb_settings_destroy().
 */
typedef struct tb_settings tb_settings;

/**
 * @brief One setting. Owned by its registry and valid until the registry is
 * destroyed.
 */
typedef struct tb_setting tb_setting;

/** @brief The longest name of a setting or a family, in bytes. */
#define TB_SETTING_NAME_LIMIT ((size_t)64)

/** @brief The longest text a text setting holds when its definition sets no
 * limit, in bytes. */
#define TB_SETTING_DEFAULT_TEXT_LIMIT ((size_t)256)

/** @brief The most choices a choice setting may have. */
#define TB_SETTING_CHOICE_LIMIT ((size_t)64)

/** @brief The largest settings file read, in bytes. */
#define TB_SETTINGS_FILE_LIMIT ((size_t)1 << 20)

/** @brief Bytes of a problem's message, its NUL included. */
#define TB_SETTINGS_MESSAGE_CAPACITY ((size_t)256)

/** @brief The kinds of value a setting holds. */
typedef enum tb_setting_type
{
    TB_SETTING_BOOLEAN, /**< On or off. */
    TB_SETTING_INTEGER, /**< A whole number within a range. */
    TB_SETTING_NUMBER,  /**< A finite real number within a range. */
    TB_SETTING_CHOICE,  /**< One of a list of named choices. */
    TB_SETTING_TEXT     /**< UTF-8 text of a limited length. */
} tb_setting_type;

/** @brief Where a setting's value comes from, lowest first; see the header's
 * description. */
typedef enum tb_setting_layer
{
    TB_SETTING_LAYER_BUILT_IN,     /**< The definition's default. Always present. */
    TB_SETTING_LAYER_DEFAULT_FILE, /**< The game's file of defaults. */
    TB_SETTING_LAYER_PLAYER_FILE,  /**< The player's settings file. */
    TB_SETTING_LAYER_COMMAND_LINE, /**< The command line, for one session. */
    TB_SETTING_LAYER_RUNTIME,      /**< A change while the game runs. */
    TB_SETTING_LAYER_COUNT         /**< The number of layers; not a layer. */
} tb_setting_layer;

/** @brief What setting a value came to. */
typedef enum tb_setting_result
{
    TB_SETTING_OK,          /**< The value is set. */
    TB_SETTING_UNKNOWN,     /**< No setting has the name. */
    TB_SETTING_INVALID,     /**< The text is not a value of the setting's type. */
    TB_SETTING_OUT_OF_RANGE /**< The value is of the type, but outside the range or the choices,
                               or text too long. */
} tb_setting_result;

/** @brief A family of settings, as an options menu groups them. */
typedef struct tb_settings_family
{
    /** One word of lowercase letters, digits and underscores: "audio". */
    const char *name;
    /** What a menu calls it: "Audio". NULL for the name. */
    const char *title;
    /** One or more sentences; NULL for none. */
    const char *description;
    /** Whether players see it in options menus and its settings are kept in
     * the player's file. Engine tuning and developer switches are not. */
    bool for_players;
} tb_settings_family;

/** @brief How a setting is defined. Exactly the member that `type` names of
 * the anonymous union is read. */
typedef struct tb_setting_definition
{
    /** Its name, whose first word is its family: "audio.music_volume". */
    const char *name;
    /** One or more sentences, for a menu's help and for a console; NULL for
     * none. */
    const char *description;
    tb_setting_type type; /**< The kind of value it holds. */
    union {
        /** For #TB_SETTING_BOOLEAN. */
        struct
        {
            bool default_value; /**< Its built-in value. */
        } boolean;
        /** For #TB_SETTING_INTEGER. */
        struct
        {
            int64_t default_value; /**< Its built-in value; within the range. */
            int64_t minimum;       /**< The least value accepted. */
            int64_t maximum;       /**< The greatest value accepted; at least `minimum`. */
        } integer;
        /** For #TB_SETTING_NUMBER. */
        struct
        {
            double default_value; /**< Its built-in value; within the range. */
            double minimum;       /**< The least value accepted; finite. */
            double maximum;       /**< The greatest value accepted; finite, at least `minimum`. */
        } number;
        /** For #TB_SETTING_CHOICE. */
        struct
        {
            /** The choices' names, each a word as a family's name is. */
            const char *const *names;
            size_t count;         /**< 1 to #TB_SETTING_CHOICE_LIMIT. */
            size_t default_value; /**< The index of its built-in choice. */
        } choice;
        /** For #TB_SETTING_TEXT. */
        struct
        {
            const char *default_value; /**< Its built-in text, UTF-8; NULL for empty. */
            /** The longest text accepted, in bytes; 0 for
             * #TB_SETTING_DEFAULT_TEXT_LIMIT. */
            size_t limit;
        } text;
    };
} tb_setting_definition;

/** @brief A problem found in settings text, for a report. */
typedef struct tb_settings_problem
{
    tb_setting_layer layer;   /**< The layer being loaded. */
    size_t line;              /**< The line, from 1. */
    tb_setting_result result; /**< What was wrong: #TB_SETTING_INVALID for a line that is not
                                 `name = value`, else as setting the value gave. */
    /** What is wrong, in UTF-8: "line 4: 'display.vsync' has no choice 'maybe'". */
    char message[TB_SETTINGS_MESSAGE_CAPACITY];
} tb_settings_problem;

/**
 * @brief Receives each problem that loading settings finds.
 * @param context What was given to the load.
 * @param problem The problem; valid only during the call.
 */
typedef void tb_settings_report_function(void *context, const tb_settings_problem *problem);

/* ---------------------------------------------------------------------------
 * The registry.
 * ------------------------------------------------------------------------- */

/**
 * @brief Create an empty registry, with no families and no settings.
 * @return The registry; never NULL. The caller owns it and releases it with
 * tb_settings_destroy().
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_settings *tb_settings_create(void);

/**
 * @brief Destroy a registry and every setting in it.
 * @param settings Registry to destroy; NULL is ignored. Every setting pointer
 * it gave is invalid afterwards.
 */
TB_API void tb_settings_destroy(tb_settings *settings);

/**
 * @brief Add a family, for the settings whose names begin with its name.
 *
 * Families keep the order they were added in, which is the order an options
 * menu shows them and a settings file lists them.
 *
 * @param settings Required non-null.
 * @param family Its definition; required. The strings are copied. A name that
 * is not one word, or that a family already has, is a contract violation.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void tb_settings_add_family(tb_settings *settings, const tb_settings_family *family);

/** @brief The number of families added. @param settings Required non-null. */
TB_API size_t tb_settings_family_count(const tb_settings *settings);

/**
 * @brief A family, by the order it was added in.
 * @param settings Required non-null.
 * @param index Less than tb_settings_family_count().
 * @return The family. Its strings are the registry's, valid until it is
 * destroyed; `title` is never NULL and `description` is NULL for none.
 */
TB_API tb_settings_family tb_settings_family_at(const tb_settings *settings, size_t index);

/**
 * @brief Add a setting.
 *
 * Settings keep the order they were added in, within their family, which is
 * the order an options menu shows them and a settings file lists them. A
 * value that a loaded file gave for the name before the setting existed
 * applies now, when it fits, and is dropped when it does not.
 *
 * @param settings Required non-null.
 * @param definition Its definition; required. Strings are copied. A wrong
 * definition is a contract violation: a name that is not valid, that a
 * setting already has, or whose family was never added; a range whose
 * minimum is above its maximum, or whose default is outside it; a choice
 * name that is not a word, or the same as another; a default text that is
 * not valid UTF-8 or longer than its limit.
 * @return The setting, owned by the registry; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_setting *tb_settings_add(tb_settings *settings, const tb_setting_definition *definition);

/** @brief The number of settings added. @param settings Required non-null. */
TB_API size_t tb_settings_count(const tb_settings *settings);

/**
 * @brief A setting, by the order it was added in.
 * @param settings Required non-null.
 * @param index Less than tb_settings_count().
 * @return The setting; never NULL.
 */
TB_API tb_setting *tb_settings_at(const tb_settings *settings, size_t index);

/**
 * @brief Find a setting by its name.
 * @param settings Required non-null.
 * @param name Required; any text.
 * @return The setting, or NULL when none has the name.
 */
TB_API tb_setting *tb_settings_find(const tb_settings *settings, const char *name);

/**
 * @brief The registry's version: one more each time any setting's value
 * changes.
 * @param settings Required non-null.
 */
TB_API uint64_t tb_settings_version(const tb_settings *settings);

/* ---------------------------------------------------------------------------
 * What a setting is.
 * ------------------------------------------------------------------------- */

/** @brief Its name, valid while the registry lives. @param setting Required non-null. */
TB_API const char *tb_setting_name(const tb_setting *setting);

/** @brief Its description, or NULL for none. @param setting Required non-null. */
TB_API const char *tb_setting_description(const tb_setting *setting);

/** @brief The kind of value it holds. @param setting Required non-null. */
TB_API tb_setting_type tb_setting_type_of(const tb_setting *setting);

/** @brief The index of its family, for tb_settings_family_at().
 * @param setting Required non-null. */
TB_API size_t tb_setting_family(const tb_setting *setting);

/** @brief Whether players see it and its value is kept in the player's file,
 * as its family says. @param setting Required non-null. */
TB_API bool tb_setting_for_players(const tb_setting *setting);

/**
 * @brief The range of an integer setting.
 * @param setting Required non-null; an integer setting.
 * @param[out] minimum Receives the least value; required.
 * @param[out] maximum Receives the greatest value; required.
 */
TB_API void tb_setting_integer_range(const tb_setting *setting, int64_t *minimum, int64_t *maximum);

/**
 * @brief The range of a number setting.
 * @param setting Required non-null; a number setting.
 * @param[out] minimum Receives the least value; required.
 * @param[out] maximum Receives the greatest value; required.
 */
TB_API void tb_setting_number_range(const tb_setting *setting, double *minimum, double *maximum);

/** @brief The number of a choice setting's choices.
 * @param setting Required non-null; a choice setting. */
TB_API size_t tb_setting_choice_count(const tb_setting *setting);

/**
 * @brief The name of one of a choice setting's choices.
 * @param setting Required non-null; a choice setting.
 * @param index Less than tb_setting_choice_count().
 * @return The name, valid while the registry lives.
 */
TB_API const char *tb_setting_choice_name(const tb_setting *setting, size_t index);

/**
 * @brief The index of a choice setting's choice with a name.
 * @param setting Required non-null; a choice setting.
 * @param name Required; any text.
 * @return The index, or tb_setting_choice_count() when no choice has the name.
 */
TB_API size_t tb_setting_find_choice(const tb_setting *setting, const char *name);

/** @brief The longest text a text setting accepts, in bytes.
 * @param setting Required non-null; a text setting. */
TB_API size_t tb_setting_text_limit(const tb_setting *setting);

/* ---------------------------------------------------------------------------
 * Reading a value.
 * ------------------------------------------------------------------------- */

/** @brief A boolean setting's value. @param setting Required non-null; a boolean setting. */
TB_API bool tb_setting_boolean(const tb_setting *setting);

/** @brief An integer setting's value. @param setting Required non-null; an integer setting. */
TB_API int64_t tb_setting_integer(const tb_setting *setting);

/** @brief A number setting's value. @param setting Required non-null; a number setting. */
TB_API double tb_setting_number(const tb_setting *setting);

/** @brief The index of a choice setting's choice.
 * @param setting Required non-null; a choice setting. */
TB_API size_t tb_setting_choice(const tb_setting *setting);

/**
 * @brief A text setting's value.
 * @param setting Required non-null; a text setting.
 * @param[out] buffer Receives the text and a NUL, when it fits; NULL only
 * when `capacity` is 0.
 * @param capacity Bytes of `buffer`. tb_setting_text_limit() plus one always
 * fits.
 * @return The text's length, without the NUL. When it is not less than
 * `capacity`, nothing is written.
 */
TB_API size_t tb_setting_text(const tb_setting *setting, char *buffer, size_t capacity);

/**
 * @brief Any setting's value as a file writes it: "true", "144", "0.5",
 * "borderless", "\"fr-FR\"".
 * @param setting Required non-null.
 * @param layer The layer whose value to write, or #TB_SETTING_LAYER_COUNT for
 * the setting's value. A layer that has no value writes nothing and returns 0.
 * @param[out] buffer Receives the text and a NUL, when it fits; NULL only
 * when `capacity` is 0.
 * @param capacity Bytes of `buffer`.
 * @return The text's length, without the NUL. When it is not less than
 * `capacity`, nothing is written.
 */
TB_API size_t tb_setting_format(const tb_setting *setting, tb_setting_layer layer, char *buffer,
                                size_t capacity);

/**
 * @brief The layer the value comes from: the highest that has one.
 * @param setting Required non-null.
 */
TB_API tb_setting_layer tb_setting_source(const tb_setting *setting);

/**
 * @brief Whether a layer has a value for a setting.
 * @param setting Required non-null.
 * @param layer A layer; #TB_SETTING_LAYER_BUILT_IN always has one.
 */
TB_API bool tb_setting_has_layer(const tb_setting *setting, tb_setting_layer layer);

/**
 * @brief The setting's version: one more each time its value changes.
 *
 * A change to a layer that a higher layer hides changes no value, and no
 * version.
 *
 * @param setting Required non-null.
 */
TB_API uint64_t tb_setting_version(const tb_setting *setting);

/* ---------------------------------------------------------------------------
 * Changing a value.
 * ------------------------------------------------------------------------- */

/**
 * @brief Set a boolean setting at the runtime layer.
 * @param setting Required non-null; a boolean setting.
 * @param value The value.
 * @return #TB_SETTING_OK.
 */
TB_API tb_setting_result tb_setting_set_boolean(tb_setting *setting, bool value);

/**
 * @brief Set an integer setting at the runtime layer.
 * @param setting Required non-null; an integer setting.
 * @param value The value.
 * @return #TB_SETTING_OK, or #TB_SETTING_OUT_OF_RANGE with nothing changed.
 */
TB_API tb_setting_result tb_setting_set_integer(tb_setting *setting, int64_t value);

/**
 * @brief Set a number setting at the runtime layer.
 * @param setting Required non-null; a number setting.
 * @param value The value.
 * @return #TB_SETTING_OK, or #TB_SETTING_OUT_OF_RANGE with nothing changed
 * for a value outside the range or not finite.
 */
TB_API tb_setting_result tb_setting_set_number(tb_setting *setting, double value);

/**
 * @brief Set a choice setting at the runtime layer.
 * @param setting Required non-null; a choice setting.
 * @param index The index of the choice.
 * @return #TB_SETTING_OK, or #TB_SETTING_OUT_OF_RANGE with nothing changed
 * for an index that is no choice's.
 */
TB_API tb_setting_result tb_setting_set_choice(tb_setting *setting, size_t index);

/**
 * @brief Set a text setting at the runtime layer.
 * @param setting Required non-null; a text setting.
 * @param text The text; required.
 * @return #TB_SETTING_OK; #TB_SETTING_INVALID for text that is not valid
 * UTF-8, or #TB_SETTING_OUT_OF_RANGE for text longer than the limit, with
 * nothing changed.
 */
TB_API tb_setting_result tb_setting_set_text(tb_setting *setting, const char *text);

/**
 * @brief Set any setting at a layer, from text written as a file writes it.
 *
 * Quotes are optional: `"0.5"` and `0.5` are the same number.
 *
 * @param setting Required non-null.
 * @param layer The layer to set; not #TB_SETTING_LAYER_BUILT_IN.
 * @param text The value; required unless `length` is 0.
 * @param length Bytes of `text`.
 * @return #TB_SETTING_OK, or what is wrong with the text, with nothing
 * changed.
 */
TB_API tb_setting_result tb_setting_parse(tb_setting *setting, tb_setting_layer layer,
                                          const char *text, size_t length);

/**
 * @brief Set a setting by its name, at a layer, from text: what a command
 * line's `--set name=value` or a console's `set name value` does.
 * @param settings Required non-null.
 * @param layer The layer to set; not #TB_SETTING_LAYER_BUILT_IN.
 * @param name Required.
 * @param value Required; as for tb_setting_parse().
 * @return #TB_SETTING_OK; #TB_SETTING_UNKNOWN when no setting has the name,
 * with nothing kept for one added later; or what is wrong with the value.
 */
TB_API tb_setting_result tb_settings_set(tb_settings *settings, tb_setting_layer layer,
                                         const char *name, const char *value);

/**
 * @brief Remove a layer's value from a setting, so that the layers below
 * give it.
 * @param setting Required non-null.
 * @param layer A layer; not #TB_SETTING_LAYER_BUILT_IN. One that has no value
 * is left as it is.
 */
TB_API void tb_setting_clear(tb_setting *setting, tb_setting_layer layer);

/**
 * @brief Restore a setting's default: remove the player's choices, those of
 * the runtime and player file layers, so that the command line or the
 * defaults give its value.
 *
 * The player's file keeps the old value until the registry is saved.
 *
 * @param setting Required non-null.
 */
TB_API void tb_setting_restore_default(tb_setting *setting);

/**
 * @brief Restore the defaults of every setting of a family, as
 * tb_setting_restore_default() does.
 * @param settings Required non-null.
 * @param family A family's name; required. One that no family has is a
 * contract violation.
 */
TB_API void tb_settings_restore_family(tb_settings *settings, const char *family);

/**
 * @brief Restore the defaults of every setting, as tb_setting_restore_default()
 * does, and forget the entries of the player's file that name no setting.
 * @param settings Required non-null.
 */
TB_API void tb_settings_restore_all(tb_settings *settings);

/* ---------------------------------------------------------------------------
 * Files.
 * ------------------------------------------------------------------------- */

/**
 * @brief Load a layer from settings text, replacing what the layer held.
 *
 * Every setting's value at the layer is removed first, as are the entries
 * kept from before for names that no setting has; then each line of the text
 * sets its setting at the layer. Problems are reported and their lines
 * skipped; the rest of the text still loads.
 *
 * @param settings Required non-null.
 * @param layer The layer to load; not #TB_SETTING_LAYER_BUILT_IN.
 * @param text The text; required unless `length` is 0. It need not end with
 * a NUL.
 * @param length Bytes of `text`.
 * @param report Receives each problem; NULL to ignore them. It is called
 * without the registry locked, and may use it.
 * @param context Passed to `report`.
 * @return The number of problems found.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API size_t tb_settings_load_text(tb_settings *settings, tb_setting_layer layer, const char *text,
                                    size_t length, tb_settings_report_function *report,
                                    void *context);

/**
 * @brief Load a layer from a settings file, as tb_settings_load_text() does.
 * @param settings Required non-null.
 * @param layer The layer to load; not #TB_SETTING_LAYER_BUILT_IN.
 * @param path The file's UTF-8 path.
 * @param report As for tb_settings_load_text().
 * @param context Passed to `report`.
 * @return #TB_FS_OK when the file was read, whatever problems it had; or why
 * it could not be, with the layer as it was. #TB_FS_NOT_FOUND is the usual
 * answer before a player's first save, and #TB_FS_TOO_LARGE is that of a file
 * longer than #TB_SETTINGS_FILE_LIMIT.
 */
TB_API tb_fs_result tb_settings_load_file(tb_settings *settings, tb_setting_layer layer,
                                          tb_path_view path, tb_settings_report_function *report,
                                          void *context);

/**
 * @brief Write the player's choices as settings text.
 *
 * For each setting of a family for players that a player chose, at the
 * runtime layer or the player file layer, the highest of those two; then the
 * entries kept from the player's file for names that no setting has. Settings
 * are written family by family, in the order they were added.
 *
 * @param settings Required non-null.
 * @param allocator Allocates the text; NULL for the heap.
 * @param[out] length Receives the text's length, without its NUL; required.
 * @return The text, NUL-terminated; never NULL. Release it with tb_str_free()
 * and the same allocator.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API char *tb_settings_write_text(const tb_settings *settings, const tb_allocator *allocator,
                                    size_t *length);

/**
 * @brief Save the player's choices to a file, as tb_settings_write_text()
 * writes them.
 *
 * The file's directory is created when it does not exist, and the file is
 * replaced in one step: a crash while saving leaves the old file or the new
 * one, never part of either.
 *
 * @param settings Required non-null.
 * @param path The file's UTF-8 path.
 * @return #TB_FS_OK, or why the file could not be written.
 */
TB_API tb_fs_result tb_settings_save_file(const tb_settings *settings, tb_path_view path);

/**
 * @brief Where a game keeps the player's settings file:
 * `settings.cfg` in a directory named after the game, in the user's
 * configuration directory.
 *
 * | Platform | Path |
 * | --- | --- |
 * | Windows | `%APPDATA%\Burrow\settings.cfg` |
 * | Linux | `$XDG_CONFIG_HOME/Burrow/settings.cfg`, or `~/.config/Burrow/settings.cfg` |
 * | macOS | `~/Library/Preferences/Burrow/settings.cfg` |
 *
 * @param application The game's directory name: one path component, such as
 * "Burrow"; required.
 * @param allocator Allocates the path; NULL for the heap.
 * @param[out] path Receives the path, NUL-terminated, on success, to release
 * with tb_str_free() and the same allocator; NULL on failure. Required.
 * @return #TB_FS_OK, #TB_FS_INVALID for an application name that is not one
 * component, or why the configuration directory is unknown.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_fs_result tb_settings_player_file_path(const char *application,
                                                 const tb_allocator *allocator, char **path);

/**
 * @brief Move a player's settings file out of the way, to the same name with
 * `.bak` added, replacing an older one.
 *
 * What a game's command line does to start from the defaults when its
 * settings left it unusable: the file is kept for the player to look at, and
 * the game starts as if it had none.
 *
 * @param path The file's UTF-8 path.
 * @return #TB_FS_OK when it was moved or there was none, or why it could not
 * be moved.
 */
TB_API tb_fs_result tb_settings_set_aside_file(tb_path_view path);

/** @brief Name a result, for a log: "out of range".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL. */
TB_API const char *tb_setting_result_name(tb_setting_result result);

/** @brief Name a layer, for a log or a listing: "command line".
 * @param layer Any value; one that is no layer is named "unknown layer".
 * @return A string with static storage duration; never NULL. */
TB_API const char *tb_setting_layer_name(tb_setting_layer layer);

#endif
