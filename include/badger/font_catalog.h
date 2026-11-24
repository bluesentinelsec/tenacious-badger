/** @file font_catalog.h
 * @brief The fonts the engine carries, a catalog to find fonts in by family,
 * weight, and slant, and the roles that name the font for each use: UI,
 * body text, headings, code, and pixel art.
 *
 * **Embedded fonts.** The engine library holds the fonts of
 * `resources/fonts/` in its own data, compressed with DEFLATE, so every
 * program that links it has fonts without a file beside it. Which of them a
 * build holds is the CMake option `TB_EMBED_FONTS`: `all` (the default), the
 * families of the default roles alone (`defaults`), or `none`. A font is
 * decompressed the first time its bytes are asked for, once for the process,
 * into memory the engine keeps until the process ends; the bytes are ready
 * for tb_font_open(). tb_font_embedded_count() says how many fonts this build
 * holds.
 *
 * **A catalog** lists the embedded fonts and any that a game adds from bytes
 * of its own, such as a file read through the VFS, and finds the best font
 * for a family, weight, and slant. It also holds the role table: the font
 * that each #tb_font_role names, which starts as the engine's defaults and
 * which a game may change. A game that wants none of this opens its own
 * bytes with tb_font_open() and never makes a catalog.
 *
 * **Finding a font.** Families are compared without regard to the case of
 * ASCII letters. Within the family, a font of the slant asked for is
 * preferred to one of the other slant, and among those the weight is chosen
 * as CSS chooses it: asked for a weight from 400 to 500, the weights from it
 * up to 500 are tried first, nearest first, then lighter ones, then heavier
 * ones; asked for less than 400, lighter ones first, then heavier; asked for
 * more than 500, heavier ones first, then lighter. Nothing is synthesized: a
 * family without italics gives its upright font, as it is. Of two fonts of
 * the same family, weight, and slant, the one added last is chosen, so a
 * game's font takes the place of an embedded one.
 *
 * **Licenses.** The embedded fonts are under the SIL Open Font License 1.1,
 * except Px437 IBM VGA 8x16, which is under CC BY-SA 4.0. Their texts are in
 * `resources/fonts/licenses/`, and ship with the engine's notices; a game
 * that ships the engine ships them too. See docs/fonts.md.
 *
 * @par Threading
 * The `tb_font_embedded_*` functions, and the names of enumerators, are
 * thread-safe: the first threads to ask for one font's bytes at once
 * decompress it once between them. A catalog's functions that take a `const
 * tb_font_catalog *` are thread-safe with one another. tb_font_catalog_add(),
 * tb_font_catalog_set_role(), and tb_font_catalog_destroy() are externally
 * synchronized: nothing else may use the catalog while they run.
 */
#ifndef TB_FONT_CATALOG_H
#define TB_FONT_CATALOG_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief What kind of letters a font draws. */
typedef enum tb_font_category
{
    TB_FONT_CATEGORY_SERIF, /**< Letters with serifs, for reading at length. */
    TB_FONT_CATEGORY_SANS,  /**< Letters without serifs, for interfaces. */
    TB_FONT_CATEGORY_MONO,  /**< Every glyph the same width, for code and logs. */
    TB_FONT_CATEGORY_RETRO, /**< Pixel and terminal fonts of old machines. */
    TB_FONT_CATEGORY_OTHER  /**< Anything else, such as a display face a game adds. */
} tb_font_category;

/** @brief The license a font is under. */
typedef enum tb_font_license
{
    TB_FONT_LICENSE_OFL_1_1,      /**< SIL Open Font License 1.1. */
    TB_FONT_LICENSE_CC_BY_SA_4_0, /**< Creative Commons Attribution-ShareAlike 4.0. */
    TB_FONT_LICENSE_GAME          /**< A font a game added; its license is the game's affair. */
} tb_font_license;

/** @brief What a font is for: each role names one family, weight, and slant. */
typedef enum tb_font_role
{
    TB_FONT_ROLE_UI,        /**< Labels, buttons, and menus. */
    TB_FONT_ROLE_UI_BOLD,   /**< Emphasis in an interface: a selected item, a title bar. */
    TB_FONT_ROLE_BODY,      /**< Paragraphs: dialogue, descriptions, help. */
    TB_FONT_ROLE_HEADING,   /**< Headings and titles. */
    TB_FONT_ROLE_MONOSPACE, /**< The console, code, logs, and numbers in columns. */
    TB_FONT_ROLE_PIXEL,     /**< Pixel-art text, drawn at whole multiples of its design size. */
    TB_FONT_ROLE_COUNT      /**< How many roles there are; not a role. */
} tb_font_role;

/** @brief What a font of a catalog, or of the embedded fonts, is. */
typedef struct tb_font_info
{
    /** The family, such as "Inter", as tb_font_get_style() reports it. For
     * an embedded font, static; for an added one, the catalog's, valid
     * until it is destroyed. */
    const char *family;
    /** The style within the family, such as "SemiBold Italic"; as `family`. */
    const char *style;
    uint16_t weight;           /**< Such as #TB_FONT_WEIGHT_REGULAR. */
    bool italic;               /**< Whether the font is italic or oblique. */
    tb_font_category category; /**< What kind of letters it draws. */
    tb_font_license license;   /**< The license it is under. */
    /** For an embedded font, its file under `resources/fonts/`, such as
     * "sans/Inter-Regular.ttf"; static. "" for a font a game added. */
    const char *file;
    bool embedded; /**< Whether the engine carries the font, rather than a game adding it. */
    /** For a pixel font, the size in pixels to the em at which one of its
     * pixels is one pixel of the screen, such as 16; draw it at whole
     * multiples of that. 0 for a font that scales freely. */
    uint16_t pixel_size;
    size_t length; /**< Bytes of the font's `.ttf` file, decompressed. */
} tb_font_info;

/** @brief The font a role names, which a catalog finds as tb_font_catalog_find() does. */
typedef struct tb_font_role_choice
{
    /** The family. Borrowed by tb_font_catalog_set_role(), which copies it;
     * from tb_font_catalog_get_role(), the catalog's, valid until the role
     * is set again or the catalog is destroyed. */
    const char *family;
    uint16_t weight; /**< The weight, such as #TB_FONT_WEIGHT_BOLD. */
    bool italic;     /**< Whether an italic is wanted. */
} tb_font_role_choice;

/**
 * @brief How many fonts this build of the engine holds.
 * @return From 0, in a build with `TB_EMBED_FONTS=none`, to every font of
 * `resources/fonts/`. The same for the life of the process.
 * @par Threading Thread-safe.
 */
TB_API size_t tb_font_embedded_count(void);

/**
 * @brief Describe an embedded font, without decompressing it.
 * @param index Less than tb_font_embedded_count(). Fonts are in the order of
 * their files' names, such as "mono/FiraCode-Bold.ttf".
 * @param[out] info Required. Its strings are static.
 * @par Threading Thread-safe.
 */
TB_API void tb_font_embedded_get_info(size_t index, tb_font_info *info);

/**
 * @brief Get the bytes of an embedded font's `.ttf` file, decompressing them
 * the first time they are asked for.
 *
 * The bytes are decompressed once for the process, into memory the engine
 * owns and never releases, so they are ready for tb_font_open() and stay
 * valid, and unchanged, until the process ends.
 *
 * @param index Less than tb_font_embedded_count().
 * @param[out] bytes Required; the bytes, borrowed for the life of the process.
 * @param[out] length Required; how many there are.
 * @par Errors Running out of memory, or data the build damaged, ends the process.
 * @par Threading Thread-safe. Threads that ask for the same font at once
 * while it is decompressed wait for the one that decompresses it.
 */
TB_API void tb_font_embedded_get_bytes(size_t index, const void **bytes, size_t *length);

/**
 * @brief Learn which font the engine's default role table names for a role.
 *
 * The defaults are provisional: the table, in src/font/font_catalog.c, is
 * one line a role, and the CMake list `TB_FONT_DEFAULT_FAMILIES` names the
 * families that `TB_EMBED_FONTS=defaults` keeps for it.
 *
 * @param role Any role but #TB_FONT_ROLE_COUNT.
 * @return The family, weight, and slant; the family is static.
 * @par Threading Thread-safe.
 */
TB_API tb_font_role_choice tb_font_default_role(tb_font_role role);

/** @brief A catalog of fonts, and the fonts its roles name. Opaque. */
typedef struct tb_font_catalog tb_font_catalog;

/**
 * @brief Make a catalog of the embedded fonts, with the default roles.
 * @param allocator Where the catalog, and the fonts added to it, live; NULL
 * for the heap.
 * @return The catalog, for tb_font_catalog_destroy().
 * @par Errors Running out of memory ends the process.
 * @par Threading Thread-safe; the catalog is the caller's.
 */
TB_API tb_font_catalog *tb_font_catalog_create(const tb_allocator *allocator);

/**
 * @brief Destroy a catalog, and the bytes of the fonts added to it.
 *
 * Fonts opened from an added font's bytes must be closed first. Embedded
 * fonts' bytes are not the catalog's, and stay valid.
 *
 * @param catalog Optional; NULL does nothing.
 */
TB_API void tb_font_catalog_destroy(tb_font_catalog *catalog);

/**
 * @brief How many fonts a catalog holds: the embedded fonts, then those added.
 * @param catalog Required.
 */
TB_API size_t tb_font_catalog_count(const tb_font_catalog *catalog);

/**
 * @brief Describe a font of a catalog.
 * @param catalog Required.
 * @param index Less than tb_font_catalog_count(). Embedded fonts come first,
 * in the order of tb_font_embedded_get_info(), then added ones, in the order
 * they were added.
 * @param[out] info Required.
 */
TB_API void tb_font_catalog_get_info(const tb_font_catalog *catalog, size_t index,
                                     tb_font_info *info);

/**
 * @brief Get the bytes of a font of a catalog, ready for tb_font_open().
 * @param catalog Required.
 * @param index Less than tb_font_catalog_count().
 * @param[out] bytes Required. An embedded font's are borrowed for the life of
 * the process; an added font's until the catalog is destroyed.
 * @param[out] length Required; how many bytes there are.
 * @par Errors As tb_font_embedded_get_bytes().
 */
TB_API void tb_font_catalog_get_bytes(const tb_font_catalog *catalog, size_t index,
                                      const void **bytes, size_t *length);

/**
 * @brief Find the font of a family that best matches a weight and a slant.
 * @param catalog Required.
 * @param family Required; a family, such as "Inter", in any case of ASCII letters.
 * @param weight The weight wanted, such as #TB_FONT_WEIGHT_SEMI_BOLD.
 * @param italic Whether an italic is wanted.
 * @param[out] index Required; the font's index on true, unchanged on false.
 * @return true when the catalog holds a font of the family, which is then
 * the best match as the file comment describes; false when it holds none.
 */
TB_API bool tb_font_catalog_find(const tb_font_catalog *catalog, const char *family,
                                 uint16_t weight, bool italic, size_t *index);

/**
 * @brief Add a font to a catalog, from the bytes of its `.ttf` file.
 *
 * The font is opened to learn its family, style, weight, and slant, which
 * the catalog then finds it by, and closed again. The catalog keeps a copy of
 * the bytes, so the caller's need not outlive the call.
 *
 * @param catalog Required.
 * @param bytes Required; borrowed for the call.
 * @param length Bytes of the file.
 * @param category What kind of letters the font draws, for its info.
 * @param[out] index Optional; the new font's index on OK.
 * @param[out] error Optional; why the font was refused, or OK and "".
 * @return OK, or why tb_font_open() refused the font; MALFORMED, with the
 * table "name", for a font that names no family, which nothing could find.
 * On failure the catalog is unchanged.
 * @par Errors Running out of memory ends the process.
 * @par Threading Externally synchronized.
 */
TB_API tb_font_result tb_font_catalog_add(tb_font_catalog *catalog, const void *bytes,
                                          size_t length, tb_font_category category, size_t *index,
                                          tb_font_error *error);

/**
 * @brief Say which font a role names.
 *
 * A role names a family, weight, and slant, not a font, so it may name a
 * font the catalog does not hold yet, and finds it once it is added.
 *
 * @param catalog Required.
 * @param role Any role but #TB_FONT_ROLE_COUNT.
 * @param choice Required; its family is required, and is copied.
 * @par Errors Running out of memory ends the process.
 * @par Threading Externally synchronized.
 */
TB_API void tb_font_catalog_set_role(tb_font_catalog *catalog, tb_font_role role,
                                     const tb_font_role_choice *choice);

/**
 * @brief Learn which font a role names.
 * @param catalog Required.
 * @param role Any role but #TB_FONT_ROLE_COUNT.
 * @return The choice, whose family is the catalog's.
 */
TB_API tb_font_role_choice tb_font_catalog_get_role(const tb_font_catalog *catalog,
                                                    tb_font_role role);

/**
 * @brief Find the font a role names, as tb_font_catalog_find() would.
 * @param catalog Required.
 * @param role Any role but #TB_FONT_ROLE_COUNT.
 * @param[out] index Required; the font's index on true, unchanged on false.
 * @return true when the catalog holds a font of the role's family.
 */
TB_API bool tb_font_catalog_find_role(const tb_font_catalog *catalog, tb_font_role role,
                                      size_t *index);

/** @brief Name a category: "serif", "sans", "mono", "retro", "other"; else "unknown". */
TB_API const char *tb_font_category_name(tb_font_category category);

/** @brief Name a license: "OFL-1.1", "CC-BY-SA-4.0", "game"; else "unknown". SPDX where it can. */
TB_API const char *tb_font_license_name(tb_font_license license);

/** @brief Name a role: "ui", "ui-bold", "body", "heading", "monospace", "pixel"; else "unknown". */
TB_API const char *tb_font_role_name(tb_font_role role);

#endif
