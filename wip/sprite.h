/** @file sprite.h
 * @brief Sprite sheets: the named frames of an image, cut by a grid or
 * listed one by one, the animations that play them, the files that
 * describe them, and atlases that pack many bitmaps into one.
 *
 * A sheet knows rectangles, not pixels: its frames are rectangles of an
 * image that the caller loads (badger/bitmap.h) and draws from, such as a
 * canvas sprite's source rectangle. A frame may be trimmed: its rectangle
 * holds only the pixels that are not transparent, and the frame says where
 * they were in the untrimmed image, so that it draws where it was drawn.
 *
 * @code
 * tb_sprite_sheet *sheet = tb_sprite_sheet_create(NULL);
 * tb_sprite_error error = {0};
 * if (tb_sprite_sheet_load_file(sheet, vfs, "sprites/hero.sprites.json", &error) != TB_SPRITE_OK)
 * {
 *     tb_log_printf(log, TB_LOG_ERROR, "media", "%s", error.message);
 * }
 * const size_t walk = tb_sprite_sheet_find_animation(sheet, "walk");
 * const size_t frame = tb_sprite_sheet_frame_at_time(sheet, walk, seconds);
 * const tb_sprite_frame *const cell = tb_sprite_sheet_frame(sheet, frame);
 * // draw cell->rect of the image, at cell->offset_x, cell->offset_y
 * @endcode
 *
 * @par Threading
 * A sheet is externally synchronized: any number of threads may read one
 * while none changes it. Packing an atlas is thread-safe for as long as the
 * bitmaps it reads are not changed, and as the allocator given is.
 */
#ifndef TB_SPRITE_H
#define TB_SPRITE_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/bitmap.h"
#include "badger/stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_vfs;

/** @brief What a function on sheets came to. */
typedef enum tb_sprite_result
{
    TB_SPRITE_OK,           /**< Done. */
    TB_SPRITE_INVALID,      /**< An argument is malformed: an empty name, a rectangle of no size. */
    TB_SPRITE_NAME_TAKEN,   /**< A frame or animation of the name is already in the sheet. */
    TB_SPRITE_NOT_FOUND,    /**< No frame of the name, or the VFS has no such file. */
    TB_SPRITE_MALFORMED,    /**< A file that is not JSON. */
    TB_SPRITE_FILE_INVALID, /**< JSON that is not a sheet of the format; the error says where. */
    TB_SPRITE_UNSUPPORTED,  /**< A sheet of a form the engine does not read: rotated frames. */
    TB_SPRITE_TOO_LARGE,    /**< An atlas that does not fit in the largest size allowed. */
    TB_SPRITE_INPUT_FAILED, /**< The VFS could not be read. */
    TB_SPRITE_OUTPUT_FAILED /**< The stream refused what was written. */
} tb_sprite_result;

/** @brief A frame: a rectangle of the sheet's image, and where it was
 * before it was trimmed. */
typedef struct tb_sprite_frame
{
    const char *name;    /**< Unique in its sheet; the sheet's, until it is destroyed. */
    tb_bitmap_rect rect; /**< The pixels in the image. */
    /** Where `rect`'s top left was in the untrimmed frame: 0, 0 for a frame
     * not trimmed. */
    int32_t offset_x;
    int32_t offset_y; /**< As `offset_x`, down. */
    /** The untrimmed frame's size: `rect`'s for a frame not trimmed. */
    uint32_t source_width;
    uint32_t source_height; /**< As `source_width`, down. */
    /** The point the frame is drawn at, from the untrimmed frame's top left,
     * in pixels: its feet, say. 0, 0 unless a file or the caller sets it. */
    float pivot_x;
    float pivot_y; /**< As `pivot_x`, down. */
} tb_sprite_frame;

/** @brief How an animation plays past its last step. */
typedef enum tb_sprite_loop
{
    TB_SPRITE_LOOP,      /**< From the first step again, for ever. */
    TB_SPRITE_ONCE,      /**< Holds its last step. */
    TB_SPRITE_PING_PONG, /**< Backward to the first, then forward again, for ever, not
                            repeating either end. */
    TB_SPRITE_LOOP_COUNT /**< The number of ways. */
} tb_sprite_loop;

/** @brief One step of an animation: a frame, for a time. */
typedef struct tb_sprite_step
{
    size_t frame;         /**< The frame's index in the sheet. */
    uint32_t duration_ms; /**< How long it shows, in milliseconds; at least 1. */
} tb_sprite_step;

/** @brief An animation of a sheet. */
typedef struct tb_sprite_animation
{
    const char *name;           /**< Unique in its sheet; the sheet's. */
    const tb_sprite_step *steps; /**< Its steps, in order; the sheet's, until it changes. */
    size_t step_count;          /**< At least 1. */
    tb_sprite_loop loop;        /**< How it plays past its last step. */
    uint64_t duration_ms;       /**< The sum of the steps' durations: once through. */
} tb_sprite_animation;

/** @brief A grid of cells of one size, cut from an image: its frames, left
 * to right, then top to bottom. */
typedef struct tb_sprite_grid
{
    /** The frames' names: `name` and the cell's number from 0, as "walk_0";
     * required. */
    const char *name;
    uint32_t cell_width;  /**< Pixels across a cell; at least 1. */
    uint32_t cell_height; /**< Pixels down a cell; at least 1. */
    uint32_t margin_x;    /**< Pixels left of the first column. */
    uint32_t margin_y;    /**< Pixels above the first row. */
    uint32_t spacing_x;   /**< Pixels between columns. */
    uint32_t spacing_y;   /**< Pixels between rows. */
    /** Cells across; 0 for as many as fit in `image_width`. */
    uint32_t columns;
    /** Cells down; 0 for as many as fit in `image_height`. */
    uint32_t rows;
    /** Cells to cut, the last row's maybe short; 0 for every one. */
    uint32_t count;
    uint32_t image_width;  /**< The image's width, when `columns` is 0. */
    uint32_t image_height; /**< The image's height, when `rows` is 0. */
} tb_sprite_grid;

/** @brief A sprite sheet. Opaque; made by tb_sprite_sheet_create(). */
typedef struct tb_sprite_sheet tb_sprite_sheet;

/** @brief What tb_sprite_sheet_find_frame() and its kin return for no such name. */
#define TB_SPRITE_NONE SIZE_MAX

/**
 * @brief Make an empty sheet.
 * @param allocator Where the sheet and everything in it live, or NULL for the heap.
 * @return The sheet; never NULL. Running out of memory ends the process.
 */
TB_API tb_sprite_sheet *tb_sprite_sheet_create(const tb_allocator *allocator);

/** @brief Release a sheet and its names. @param sheet A sheet, or NULL to do nothing. */
TB_API void tb_sprite_sheet_destroy(tb_sprite_sheet *sheet);

/**
 * @brief Name the image the sheet's frames are of, as a file names it.
 * @param sheet Required. @param image A path, copied; NULL or "" for none.
 */
TB_API void tb_sprite_sheet_set_image(tb_sprite_sheet *sheet, const char *image);

/** @brief The image the sheet names; "" for none. @param sheet Required.
 * @return The sheet's, until it changes. */
TB_API const char *tb_sprite_sheet_image(const tb_sprite_sheet *sheet);

/**
 * @brief Add a frame.
 * @param sheet Required.
 * @param frame Its name, which is copied, rectangle, trimming and pivot. A
 * `source_width` or `source_height` of 0 is the rectangle's.
 * @param[out] index Its index; NULL when nobody asks.
 * @return OK; INVALID for an empty name, or a rectangle of no size or at
 * negative coordinates; NAME_TAKEN.
 */
TB_API tb_sprite_result tb_sprite_sheet_add_frame(tb_sprite_sheet *sheet,
                                                  const tb_sprite_frame *frame, size_t *index);

/**
 * @brief Cut a grid into frames, added in order.
 * @param sheet Required. @param grid Required.
 * @param[out] first The index of the first frame added; NULL when nobody asks.
 * @param[out] count How many were added; NULL when nobody asks.
 * @return OK; INVALID for a grid of no cells or no name; NAME_TAKEN, and
 * then none is added.
 */
TB_API tb_sprite_result tb_sprite_sheet_add_grid(tb_sprite_sheet *sheet, const tb_sprite_grid *grid,
                                                 size_t *first, size_t *count);

/**
 * @brief Add an animation.
 * @param sheet Required. @param name Required; copied.
 * @param steps Its steps, copied: frames of the sheet, each for at least 1 ms.
 * @param count At least 1.
 * @param loop How it plays past its last step.
 * @param[out] index Its index; NULL when nobody asks.
 * @return OK; INVALID for no steps, a frame past the sheet's or a duration
 * of 0; NAME_TAKEN.
 */
TB_API tb_sprite_result tb_sprite_sheet_add_animation(tb_sprite_sheet *sheet, const char *name,
                                                      const tb_sprite_step *steps, size_t count,
                                                      tb_sprite_loop loop, size_t *index);

/** @brief Frames of a sheet. @param sheet Required. */
TB_API size_t tb_sprite_sheet_frame_count(const tb_sprite_sheet *sheet);

/** @brief A frame. @param sheet Required. @param index Below the frame count.
 * @return The sheet's, until a frame is added. */
TB_API const tb_sprite_frame *tb_sprite_sheet_frame(const tb_sprite_sheet *sheet, size_t index);

/** @brief The index of a frame by name, or #TB_SPRITE_NONE.
 * @param sheet Required. @param name Required. */
TB_API size_t tb_sprite_sheet_find_frame(const tb_sprite_sheet *sheet, const char *name);

/** @brief Animations of a sheet. @param sheet Required. */
TB_API size_t tb_sprite_sheet_animation_count(const tb_sprite_sheet *sheet);

/** @brief An animation. @param sheet Required. @param index Below the
 * animation count. @return The sheet's, until an animation is added. */
TB_API const tb_sprite_animation *tb_sprite_sheet_animation(const tb_sprite_sheet *sheet,
                                                           size_t index);

/** @brief The index of an animation by name, or #TB_SPRITE_NONE.
 * @param sheet Required. @param name Required. */
TB_API size_t tb_sprite_sheet_find_animation(const tb_sprite_sheet *sheet, const char *name);

/**
 * @brief The step an animation shows at a time from its start, as its loop
 * says: what a player of it draws.
 * @param animation Required.
 * @param time_ms Milliseconds since it started.
 * @return The step's index in `animation->steps`.
 */
TB_API size_t tb_sprite_animation_step_at(const tb_sprite_animation *animation, uint64_t time_ms);

/* -------------------------------------------------------------------------
 * Files.
 * ------------------------------------------------------------------------- */

/** @brief The format a sheet's file names: `"format": "badger-sprites"`. */
#define TB_SPRITE_FILE_FORMAT "badger-sprites"

/** @brief The version of the format this engine writes, and the newest it reads. */
#define TB_SPRITE_FILE_VERSION 1

/** @brief Bytes of the path, and of the message, of #tb_sprite_error. */
#define TB_SPRITE_ERROR_CAPACITY 256

/** @brief Why a file was refused, and where, as the input's files say it. */
typedef struct tb_sprite_error
{
    tb_sprite_result result; /**< As the function returned. */
    /** The JSON path of the value at fault, as `$.frames[2].rect`; "" when
     * the fault is not in a value. */
    char path[TB_SPRITE_ERROR_CAPACITY];
    size_t line;        /**< In the text, from 1; 0 when the fault is not in the text. */
    size_t column;      /**< Byte in the line, from 1; 0 as `line`. */
    const char *reason; /**< A short phrase; static; "" for none. */
    /** All of it, for a log: "hero.json:12:16: $.frames[2].rect: a rectangle of no size". */
    char message[TB_SPRITE_ERROR_CAPACITY];
} tb_sprite_error;

/**
 * @brief Add the frames and animations of a file's text to a sheet, all or
 * nothing. The text is a sheet of the engine's format (see `docs/media.md`),
 * or a JSON sheet of Aseprite or TexturePacker, recognized by its members.
 * @param sheet Required.
 * @param text Strict JSON; NULL only at length 0.
 * @param length Bytes of text.
 * @param name The file's name, for messages; NULL for none.
 * @param[out] error Why it failed; NULL when nobody asks.
 * @return OK, MALFORMED, FILE_INVALID or UNSUPPORTED; on failure the sheet
 * is as it was.
 */
TB_API tb_sprite_result tb_sprite_sheet_load(tb_sprite_sheet *sheet, const char *text,
                                             size_t length, const char *name,
                                             tb_sprite_error *error);

/**
 * @brief As tb_sprite_sheet_load(), from a file of a VFS.
 * @param sheet Required. @param vfs Required. @param path Required.
 * @param[out] error Why it failed; NULL when nobody asks.
 * @return As tb_sprite_sheet_load(), or NOT_FOUND or INPUT_FAILED.
 */
TB_API tb_sprite_result tb_sprite_sheet_load_file(tb_sprite_sheet *sheet, struct tb_vfs *vfs,
                                                  const char *path, tb_sprite_error *error);

/**
 * @brief Write a sheet in the engine's format, which tb_sprite_sheet_load()
 * reads back to the same sheet. The same sheet writes the same bytes.
 * @param sheet Required. @param sink Required open stream that can write; borrowed.
 * @param[out] error Why it failed; NULL when nobody asks.
 * @return OK or OUTPUT_FAILED.
 */
TB_API tb_sprite_result tb_sprite_sheet_save(const tb_sprite_sheet *sheet, tb_stream *sink,
                                             tb_sprite_error *error);

/* -------------------------------------------------------------------------
 * Atlases.
 * ------------------------------------------------------------------------- */

/** @brief One bitmap to pack. */
typedef struct tb_sprite_atlas_input
{
    const char *name;         /**< Its frame's name; required, unique. */
    const tb_bitmap *bitmap;  /**< Required; of any format, with pixels. */
    float pivot_x;            /**< The frame's pivot, from the bitmap's top left. */
    float pivot_y;            /**< As `pivot_x`, down. */
} tb_sprite_atlas_input;

/** @brief How to pack. Zero-initialize for the defaults. */
typedef struct tb_sprite_atlas_options
{
    uint32_t max_width;  /**< The widest the atlas may be; 0 for 4096. */
    uint32_t max_height; /**< The highest; 0 for 4096. */
    /** Transparent pixels between frames, beyond their extrusion. */
    uint32_t padding;
    /** Pixels each frame's edge is repeated outward, so that filtering at
     * its edge samples the edge, not a neighbour. */
    uint32_t extrude;
    bool trim;           /**< Cut each bitmap's transparent border, remembering where it was. */
    bool power_of_two;   /**< Make each side of the atlas a power of two. */
} tb_sprite_atlas_options;

/**
 * @brief Pack bitmaps into one, and describe where each went as a frame of
 * a sheet, in the order given. Rectangles are placed by MaxRects, choosing
 * for each, largest first, the free space it fits most snugly; the atlas is
 * the smallest that holds them all that the search finds, up to the limits.
 * @param inputs Required. @param count At least 1.
 * @param options How to pack, or NULL for the defaults.
 * @param allocator Where the atlas lives, or NULL for the heap.
 * @param sheet Required; the frames are added to it, named as the inputs.
 * @param[out] atlas Required; an RGBA8 bitmap with straight alpha on OK, NULL otherwise.
 * @return OK; INVALID for an input of no pixels or no name; NAME_TAKEN;
 * TOO_LARGE when the bitmaps do not fit. On failure the sheet is as it was.
 */
TB_API tb_sprite_result tb_sprite_atlas_pack(const tb_sprite_atlas_input *inputs, size_t count,
                                             const tb_sprite_atlas_options *options,
                                             const tb_allocator *allocator,
                                             tb_sprite_sheet *sheet, tb_bitmap **atlas);

/** @brief A short name of a result, such as "name taken"; "unknown result"
 * for a value that is none. */
TB_API const char *tb_sprite_result_name(tb_sprite_result result);

#endif
