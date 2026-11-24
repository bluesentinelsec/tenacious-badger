/* The glyph atlas: rasterized glyphs packed into one image by a skyline,
 * found again by a table of what they were drawn from, and the image
 * cleared and packed anew when a glyph no longer fits.
 *
 * The skyline is the top edge of what is packed, as a list of level runs
 * from left to right. A glyph goes where its bottom would be highest up the
 * image, that is lowest in y, resting on the runs beneath it, and the runs
 * it covers become one at its bottom. Glyphs of a run of text are of
 * similar heights, so little is wasted under them. */
#include "badger/font_raster.h"

#include "badger/allocator.h"
#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/font.h"
#include "badger/hash.h"
#include "badger/table.h"
#include "font_internal.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TB_FONT_ATLAS_MIN_SIDE 16u
#define TB_FONT_ATLAS_MAX_PADDING 8u

/* What a glyph in the atlas was drawn from. */
typedef struct tb_font_atlas_key
{
    const tb_font *font;
    float pixels_per_em; /* As clamped, so that equal sizes are equal keys. */
    uint16_t glyph;
    uint16_t bin; /* The pen's fraction, in subpixel bins. */
    tb_font_raster_mode mode;
} tb_font_atlas_key;

typedef struct tb_font_atlas_entry
{
    tb_font_atlas_key key;
    tb_font_atlas_glyph placed; /* `left` as the bin draws it, before any carry. */
} tb_font_atlas_entry;

/* A level run of the skyline: from x, `width` wide, packed down to y. */
typedef struct tb_font_skyline_run
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
} tb_font_skyline_run;

struct tb_font_atlas
{
    tb_allocator allocator;
    uint32_t width;
    uint32_t height;
    uint32_t padding;
    uint32_t bins;
    uint8_t *pixels;
    tb_font_skyline_run *runs; /* At most width + 1 of them, in order of x. */
    size_t run_count;
    tb_table *glyphs;  /* tb_font_atlas_key * to tb_font_atlas_entry *. */
    tb_arena *entries; /* The entries, released when the atlas is cleared. */
    tb_arena *scratch; /* Glyphs as they are drawn. */
    tb_font_atlas_rect dirty[TB_FONT_ATLAS_MAX_DIRTY_RECTS];
    size_t dirty_count;
    uint64_t generation;
};

/* -------------------------------------------------------------------------
 * Keys.
 * ------------------------------------------------------------------------- */

static uint64_t tb_font_atlas_hash(const void *key)
{
    const tb_font_atlas_key *atlas_key = key;
    uint32_t size_bits = 0;
    memcpy(&size_bits, &atlas_key->pixels_per_em, sizeof(size_bits));
    const uint64_t packed = ((uint64_t)size_bits << 32) | ((uint64_t)atlas_key->glyph << 16) |
                            ((uint64_t)atlas_key->bin << 4) | (uint64_t)atlas_key->mode;
    return tb_hash_pointer(atlas_key->font) ^ tb_hash_u64(packed);
}

static bool tb_font_atlas_equal(const void *left, const void *right)
{
    const tb_font_atlas_key *a = left;
    const tb_font_atlas_key *b = right;
    return a->font == b->font && a->pixels_per_em == b->pixels_per_em && a->glyph == b->glyph &&
           a->bin == b->bin && a->mode == b->mode;
}

/* -------------------------------------------------------------------------
 * Changed rectangles.
 * ------------------------------------------------------------------------- */

static bool tb_font_atlas_all_dirty(const tb_font_atlas *atlas)
{
    return atlas->dirty_count == 1 && atlas->dirty[0].width == atlas->width &&
           atlas->dirty[0].height == atlas->height;
}

static tb_font_atlas_rect tb_font_atlas_union(tb_font_atlas_rect a, tb_font_atlas_rect b)
{
    const uint32_t left = a.x < b.x ? a.x : b.x;
    const uint32_t top = a.y < b.y ? a.y : b.y;
    const uint32_t a_right = a.x + a.width;
    const uint32_t b_right = b.x + b.width;
    const uint32_t a_bottom = a.y + a.height;
    const uint32_t b_bottom = b.y + b.height;
    const uint32_t right = a_right > b_right ? a_right : b_right;
    const uint32_t bottom = a_bottom > b_bottom ? a_bottom : b_bottom;
    const tb_font_atlas_rect merged = {left, top, right - left, bottom - top};
    return merged;
}

static void tb_font_atlas_mark_dirty(tb_font_atlas *atlas, tb_font_atlas_rect rect)
{
    if (tb_font_atlas_all_dirty(atlas))
    {
        return;
    }
    if (atlas->dirty_count < TB_FONT_ATLAS_MAX_DIRTY_RECTS)
    {
        atlas->dirty[atlas->dirty_count] = rect;
        atlas->dirty_count++;
        return;
    }
    tb_font_atlas_rect merged = rect;
    for (size_t index = 0; index < atlas->dirty_count; index++)
    {
        merged = tb_font_atlas_union(merged, atlas->dirty[index]);
    }
    atlas->dirty[0] = merged;
    atlas->dirty_count = 1;
}

/* -------------------------------------------------------------------------
 * The skyline.
 * ------------------------------------------------------------------------- */

/* Where a box `width` by `height` would rest with its left at run `index`:
 * the y of its top, or false when it would stick out of the image. */
static bool tb_font_skyline_fit(const tb_font_atlas *atlas, size_t index, uint32_t width,
                                uint32_t height, uint32_t *top)
{
    const uint32_t x = atlas->runs[index].x;
    if (width > atlas->width - x)
    {
        return false;
    }
    uint32_t y = 0;
    uint32_t remaining = width;
    for (size_t run = index; remaining > 0; run++)
    {
        TB_ASSERT(run < atlas->run_count);
        if (atlas->runs[run].y > y)
        {
            y = atlas->runs[run].y;
        }
        if (height > atlas->height - y)
        {
            return false;
        }
        remaining -= atlas->runs[run].width < remaining ? atlas->runs[run].width : remaining;
    }
    *top = y;
    return true;
}

static void tb_font_skyline_remove(tb_font_atlas *atlas, size_t index)
{
    memmove(&atlas->runs[index], &atlas->runs[index + 1],
            (atlas->run_count - index - 1) * sizeof(tb_font_skyline_run));
    atlas->run_count--;
}

/* Put a box on the skyline at run `index`, its top at `top`. */
static void tb_font_skyline_place(tb_font_atlas *atlas, size_t index, uint32_t top, uint32_t width,
                                  uint32_t height)
{
    TB_ASSERT(atlas->run_count <= atlas->width);
    memmove(&atlas->runs[index + 1], &atlas->runs[index],
            (atlas->run_count - index) * sizeof(tb_font_skyline_run));
    atlas->run_count++;
    const tb_font_skyline_run placed = {atlas->runs[index].x, top + height, width};
    atlas->runs[index] = placed;
    /* The runs the box covers shrink from the left, or go. */
    const uint32_t right = placed.x + placed.width;
    size_t next = index + 1;
    while (next < atlas->run_count && atlas->runs[next].x < right)
    {
        const uint32_t covered = right - atlas->runs[next].x;
        if (covered < atlas->runs[next].width)
        {
            atlas->runs[next].x += covered;
            atlas->runs[next].width -= covered;
            break;
        }
        tb_font_skyline_remove(atlas, next);
    }
    /* Neighbours at one level become one run. */
    size_t run = 0;
    while (run + 1 < atlas->run_count)
    {
        if (atlas->runs[run].y == atlas->runs[run + 1].y)
        {
            atlas->runs[run].width += atlas->runs[run + 1].width;
            tb_font_skyline_remove(atlas, run + 1);
            continue;
        }
        run++;
    }
}

/* Find room for a box, and take it: its top-left in `x` and `y`. */
static bool tb_font_skyline_pack(tb_font_atlas *atlas, uint32_t width, uint32_t height, uint32_t *x,
                                 uint32_t *y)
{
    bool found = false;
    size_t best_index = 0;
    uint32_t best_top = 0;
    uint32_t best_bottom = 0;
    for (size_t index = 0; index < atlas->run_count; index++)
    {
        uint32_t top = 0;
        if (!tb_font_skyline_fit(atlas, index, width, height, &top))
        {
            continue;
        }
        if (!found || top + height < best_bottom)
        {
            found = true;
            best_index = index;
            best_top = top;
            best_bottom = top + height;
        }
    }
    if (!found)
    {
        return false;
    }
    *x = atlas->runs[best_index].x;
    *y = best_top;
    tb_font_skyline_place(atlas, best_index, best_top, width, height);
    return true;
}

/* -------------------------------------------------------------------------
 * The atlas.
 * ------------------------------------------------------------------------- */

static uint32_t tb_font_atlas_option(uint32_t value, uint32_t default_value)
{
    return value == 0 ? default_value : value;
}

tb_font_atlas *tb_font_atlas_create(const tb_font_atlas_options *options,
                                    const tb_allocator *allocator)
{
    const tb_font_atlas_options none = {0};
    const tb_font_atlas_options *chosen = options != NULL ? options : &none;
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_font_atlas *atlas =
        tb_allocator_calloc(&resolved, 1, sizeof(tb_font_atlas), _Alignof(tb_font_atlas));
    atlas->allocator = resolved;
    atlas->width = tb_font_atlas_option(chosen->width, TB_FONT_ATLAS_DEFAULT_SIDE);
    atlas->height = tb_font_atlas_option(chosen->height, TB_FONT_ATLAS_DEFAULT_SIDE);
    atlas->padding = tb_font_atlas_option(chosen->padding, 1);
    atlas->bins = tb_font_atlas_option(chosen->subpixel_bins, 1);
    TB_ASSERT(atlas->width >= TB_FONT_ATLAS_MIN_SIDE && atlas->width <= TB_FONT_RASTER_MAX_SIDE);
    TB_ASSERT(atlas->height >= TB_FONT_ATLAS_MIN_SIDE && atlas->height <= TB_FONT_RASTER_MAX_SIDE);
    TB_ASSERT(atlas->padding <= TB_FONT_ATLAS_MAX_PADDING);
    TB_ASSERT(atlas->bins <= TB_FONT_ATLAS_MAX_SUBPIXEL_BINS);
    atlas->pixels = tb_allocator_calloc(&resolved, (size_t)atlas->width * atlas->height, 1, 1);
    atlas->runs = tb_allocator_calloc(&resolved, (size_t)atlas->width + 1,
                                      sizeof(tb_font_skyline_run), _Alignof(tb_font_skyline_run));
    atlas->glyphs = tb_table_create(&resolved, 0, tb_font_atlas_hash, tb_font_atlas_equal);
    atlas->entries = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    atlas->scratch = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    atlas->generation = 0;
    tb_font_atlas_reset(atlas);
    atlas->generation = 0;
    return atlas;
}

void tb_font_atlas_destroy(tb_font_atlas *atlas)
{
    if (atlas == NULL)
    {
        return;
    }
    const tb_allocator allocator = atlas->allocator;
    tb_arena_destroy(atlas->scratch);
    tb_arena_destroy(atlas->entries);
    tb_table_destroy(atlas->glyphs);
    tb_allocator_free_array(&allocator, atlas->runs, (size_t)atlas->width + 1,
                            sizeof(tb_font_skyline_run), _Alignof(tb_font_skyline_run));
    tb_allocator_free_array(&allocator, atlas->pixels, (size_t)atlas->width * atlas->height, 1, 1);
    tb_allocator_free_array(&allocator, atlas, 1, sizeof(tb_font_atlas), _Alignof(tb_font_atlas));
}

void tb_font_atlas_reset(tb_font_atlas *atlas)
{
    TB_ASSERT(atlas != NULL);
    tb_table_clear(atlas->glyphs);
    tb_arena_reset(atlas->entries);
    memset(atlas->pixels, 0, (size_t)atlas->width * atlas->height);
    const tb_font_skyline_run floor_run = {0, 0, atlas->width};
    atlas->runs[0] = floor_run;
    atlas->run_count = 1;
    const tb_font_atlas_rect whole = {0, 0, atlas->width, atlas->height};
    atlas->dirty[0] = whole;
    atlas->dirty_count = 1;
    atlas->generation++;
}

uint64_t tb_font_atlas_generation(const tb_font_atlas *atlas)
{
    TB_ASSERT(atlas != NULL);
    return atlas->generation;
}

size_t tb_font_atlas_glyph_count(const tb_font_atlas *atlas)
{
    TB_ASSERT(atlas != NULL);
    return tb_table_length(atlas->glyphs);
}

void tb_font_atlas_get_image(const tb_font_atlas *atlas, tb_font_atlas_image *image)
{
    TB_ASSERT(atlas != NULL);
    TB_ASSERT(image != NULL);
    image->pixels = atlas->pixels;
    image->width = atlas->width;
    image->height = atlas->height;
    image->stride = atlas->width;
}

const tb_font_atlas_rect *tb_font_atlas_get_dirty(const tb_font_atlas *atlas, size_t *count)
{
    TB_ASSERT(atlas != NULL);
    TB_ASSERT(count != NULL);
    *count = atlas->dirty_count;
    return atlas->dirty_count > 0 ? atlas->dirty : NULL;
}

void tb_font_atlas_clear_dirty(tb_font_atlas *atlas)
{
    TB_ASSERT(atlas != NULL);
    atlas->dirty_count = 0;
}

/* Copy a drawn glyph into the image at its rectangle. */
static void tb_font_atlas_copy(tb_font_atlas *atlas, const tb_font_glyph_image *image,
                               tb_font_atlas_rect rect)
{
    for (uint32_t row = 0; row < rect.height; row++)
    {
        uint8_t *to = atlas->pixels + (size_t)(rect.y + row) * atlas->width + rect.x;
        memcpy(to, image->pixels + (size_t)row * image->width, image->width);
    }
}

/* Find room for a drawn glyph, clearing the atlas when there is none. */
static tb_font_result tb_font_atlas_store(tb_font_atlas *atlas, const tb_font_glyph_image *image,
                                          tb_font_atlas_rect *rect, tb_font_error *error)
{
    *rect = (tb_font_atlas_rect){0};
    if (image->width == 0)
    {
        return TB_FONT_OK;
    }
    const uint32_t padded_width = image->width + 2 * atlas->padding;
    const uint32_t padded_height = image->height + 2 * atlas->padding;
    if (padded_width > atlas->width || padded_height > atlas->height)
    {
        return tb_font_fail(error, TB_FONT_TOO_LARGE, "", "the glyph is larger than the atlas");
    }
    uint32_t x = 0;
    uint32_t y = 0;
    if (!tb_font_skyline_pack(atlas, padded_width, padded_height, &x, &y))
    {
        tb_font_atlas_reset(atlas);
        const bool packed = tb_font_skyline_pack(atlas, padded_width, padded_height, &x, &y);
        TB_ASSERT(packed);
        (void)packed;
    }
    *rect =
        (tb_font_atlas_rect){x + atlas->padding, y + atlas->padding, image->width, image->height};
    tb_font_atlas_copy(atlas, image, *rect);
    tb_font_atlas_mark_dirty(atlas, *rect);
    return TB_FONT_OK;
}

/* Draw a glyph the atlas does not hold, and keep it. */
static tb_font_result tb_font_atlas_add(tb_font_atlas *atlas, const tb_font_atlas_key *key,
                                        const tb_font_raster_options *options,
                                        tb_font_atlas_glyph *placed, tb_font_error *error)
{
    tb_font_raster_options drawn = *options;
    drawn.pixels_per_em = key->pixels_per_em;
    drawn.subpixel_x = (float)key->bin / (float)atlas->bins;
    const tb_arena_mark mark = tb_arena_get_mark(atlas->scratch);
    tb_font_glyph_image image = {0};
    tb_font_atlas_rect rect = {0};
    tb_font_result result =
        tb_font_rasterize_glyph(key->font, key->glyph, &drawn, atlas->scratch, &image, error);
    if (result != TB_FONT_OK)
    {
        goto done;
    }
    result = tb_font_atlas_store(atlas, &image, &rect, error);
    if (result != TB_FONT_OK)
    {
        goto done;
    }
    tb_font_atlas_entry *entry = TB_ARENA_NEW(atlas->entries, tb_font_atlas_entry);
    entry->key = *key;
    entry->placed.rect = rect;
    entry->placed.left = image.left;
    entry->placed.top = image.top;
    entry->placed.advance = image.advance;
    (void)tb_table_put(atlas->glyphs, &entry->key, entry);
    *placed = entry->placed;

done:
    tb_arena_rewind(atlas->scratch, mark);
    return result;
}

tb_font_result tb_font_atlas_get_glyph(tb_font_atlas *atlas, const tb_font *font, uint16_t glyph,
                                       const tb_font_raster_options *options,
                                       tb_font_atlas_glyph *placed, tb_font_error *error)
{
    TB_ASSERT(atlas != NULL);
    TB_ASSERT(font != NULL);
    TB_ASSERT(options != NULL);
    TB_ASSERT(placed != NULL);
    *placed = (tb_font_atlas_glyph){0};
    tb_font_error local_error = {TB_FONT_OK, "", ""};

    /* The pen's fraction, to the nearest bin; the last rounds up to the
     * next whole pixel. */
    const float bins = (float)atlas->bins;
    uint32_t bin = (uint32_t)floorf(tb_font_raster_subpixel(options->subpixel_x) * bins + 0.5f);
    int32_t carry = 0;
    if (bin >= atlas->bins)
    {
        bin = 0;
        carry = 1;
    }
    tb_font_atlas_key key = {0};
    key.font = font;
    key.pixels_per_em = tb_font_raster_clamp_size(options->pixels_per_em);
    key.glyph = glyph;
    key.bin = (uint16_t)bin;
    key.mode = options->mode;
    tb_font_result result = TB_FONT_OK;
    const tb_font_atlas_entry *found = tb_table_get(atlas->glyphs, &key);
    if (found != NULL)
    {
        *placed = found->placed;
    }
    else
    {
        result = tb_font_atlas_add(atlas, &key, options, placed, &local_error);
    }
    if (result == TB_FONT_OK)
    {
        placed->left += carry;
    }
    if (error != NULL)
    {
        *error = local_error;
    }
    return result;
}
