/* Offline sprite packing. MaxRects keeps the maximal empty rectangles after
 * each placement; two free rectangles may overlap each other, but neither
 * can contain pixels already placed. */
#include "sprite_internal.h"

#include "badger/assert.h"
#include "badger/sort.h"

#include <string.h>

#define TB_ATLAS_DEFAULT_LIMIT 4096u

typedef struct tb_atlas_box
{
    uint32_t left, top, right, bottom;
} tb_atlas_box;

typedef struct tb_atlas_piece
{
    size_t index;
    uint32_t width, height; /* includes extrusion and one inter-frame gap */
    tb_atlas_box placement;
} tb_atlas_piece;

static uint32_t tb_atlas_min(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static uint32_t tb_atlas_max(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static uint32_t tb_atlas_power_of_two(uint32_t side)
{
    uint32_t power = 1;
    while (power < side)
    {
        power *= 2;
    }
    return power;
}

static uint32_t tb_atlas_limit(uint32_t requested, bool power_of_two)
{
    const uint32_t limit =
        requested == 0 ? TB_ATLAS_DEFAULT_LIMIT : tb_atlas_min(requested, TB_BITMAP_MAX_DIMENSION);
    const uint32_t power = tb_atlas_power_of_two(limit);
    return power_of_two && power > limit ? power / 2u : limit;
}

/* A completely clear frame still needs a sampleable rectangle. */
static tb_bitmap_rect tb_atlas_crop(const tb_bitmap *bitmap, const tb_bitmap_info *info, bool trim)
{
    if (!trim || info->format == TB_BITMAP_GRAY8)
    {
        return (tb_bitmap_rect){0, 0, info->width, info->height};
    }
    tb_atlas_box bounds = {info->width, info->height, 0, 0};
    for (uint32_t y = 0; y < info->height; y++)
    {
        const uint8_t *const row = tb_bitmap_const_row(bitmap, y);
        for (uint32_t x = 0; x < info->width; x++)
        {
            /* RGBA8 and BGRA8 both store alpha in the fourth byte. */
            if (row[(size_t)x * 4u + 3u] != 0)
            {
                bounds.left = tb_atlas_min(bounds.left, x);
                bounds.top = tb_atlas_min(bounds.top, y);
                bounds.right = tb_atlas_max(bounds.right, x + 1u);
                bounds.bottom = y + 1u;
            }
        }
    }
    if (bounds.right == 0)
    {
        return (tb_bitmap_rect){0, 0, 1, 1};
    }
    return (tb_bitmap_rect){(int32_t)bounds.left, (int32_t)bounds.top, bounds.right - bounds.left,
                            bounds.bottom - bounds.top};
}

static int tb_atlas_largest_first(const void *left, const void *right, void *context)
{
    const tb_atlas_piece *const a = left;
    const tb_atlas_piece *const b = right;
    const uint64_t area_a = (uint64_t)a->width * a->height;
    const uint64_t area_b = (uint64_t)b->width * b->height;
    (void)context;
    if (area_a != area_b)
    {
        return area_a > area_b ? -1 : 1;
    }
    return (a->index > b->index) - (a->index < b->index);
}

static bool tb_atlas_contains(tb_atlas_box outer, tb_atlas_box inner)
{
    return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right &&
           outer.bottom >= inner.bottom;
}

static void tb_atlas_keep_space(tb_array *spaces, tb_atlas_box box)
{
    for (size_t i = 0; i < tb_array_length(spaces);)
    {
        const tb_atlas_box other = *TB_ARRAY_AT(spaces, tb_atlas_box, i);
        if (tb_atlas_contains(other, box))
        {
            return;
        }
        if (tb_atlas_contains(box, other))
        {
            tb_array_remove_unordered(spaces, i, NULL);
        }
        else
        {
            i++;
        }
    }
    TB_ARRAY_PUSH(spaces, tb_atlas_box, box);
}

static void tb_atlas_split(tb_array *spaces, tb_atlas_box box, tb_atlas_box used)
{
    if (box.right <= used.left || box.left >= used.right || box.bottom <= used.top ||
        box.top >= used.bottom)
    {
        tb_atlas_keep_space(spaces, box);
        return;
    }
    if (box.left < used.left)
    {
        tb_atlas_keep_space(spaces, (tb_atlas_box){box.left, box.top, used.left, box.bottom});
    }
    if (box.right > used.right)
    {
        tb_atlas_keep_space(spaces, (tb_atlas_box){used.right, box.top, box.right, box.bottom});
    }
    if (box.top < used.top)
    {
        tb_atlas_keep_space(spaces, (tb_atlas_box){box.left, box.top, box.right, used.top});
    }
    if (box.bottom > used.bottom)
    {
        tb_atlas_keep_space(spaces, (tb_atlas_box){box.left, used.bottom, box.right, box.bottom});
    }
}

static bool tb_atlas_place(tb_atlas_piece *piece, const tb_array *spaces)
{
    uint32_t shortest = UINT32_MAX;
    uint32_t longest = UINT32_MAX;
    const size_t count = tb_array_length(spaces);
    for (size_t i = 0; i < count; i++)
    {
        const tb_atlas_box box = *TB_ARRAY_AT(spaces, tb_atlas_box, i);
        const uint32_t width = box.right - box.left;
        const uint32_t height = box.bottom - box.top;
        if (width < piece->width || height < piece->height)
        {
            continue;
        }
        const uint32_t dx = width - piece->width;
        const uint32_t dy = height - piece->height;
        const uint32_t small = tb_atlas_min(dx, dy);
        const uint32_t large = tb_atlas_max(dx, dy);
        if (small < shortest || (small == shortest && large < longest))
        {
            shortest = small;
            longest = large;
            piece->placement =
                (tb_atlas_box){box.left, box.top, box.left + piece->width, box.top + piece->height};
        }
    }
    return shortest != UINT32_MAX;
}

static bool tb_atlas_arrange(tb_atlas_piece *pieces, size_t count, tb_atlas_box bin,
                             tb_array *spaces, tb_array *next, tb_atlas_box *extent)
{
    tb_array_clear(spaces);
    TB_ARRAY_PUSH(spaces, tb_atlas_box, bin);
    for (size_t i = 0; i < count; i++)
    {
        if (!tb_atlas_place(&pieces[i], spaces))
        {
            return false;
        }
        const tb_atlas_box used = pieces[i].placement;
        extent->right = tb_atlas_max(extent->right, used.right);
        extent->bottom = tb_atlas_max(extent->bottom, used.bottom);
        tb_array_clear(next);
        for (size_t j = 0; j < tb_array_length(spaces); j++)
        {
            tb_atlas_split(next, *TB_ARRAY_AT(spaces, tb_atlas_box, j), used);
        }
        tb_array *const previous = spaces;
        spaces = next;
        next = previous;
    }
    return true;
}

/* Search narrow bins as well as the full width. Crop each successful layout
 * to its used bounds before comparing areas. The virtual bin extends one
 * padding beyond each limit, so there is no compulsory outer gutter. */
static tb_atlas_box tb_atlas_layout(tb_atlas_piece *pieces, size_t count, tb_sprite_frame *frames,
                                    tb_atlas_box limit, const tb_sprite_atlas_options *options,
                                    uint32_t gap, const tb_allocator *allocator)
{
    tb_array *const spaces = TB_ARRAY_CREATE(allocator, tb_atlas_box);
    tb_array *const next = TB_ARRAY_CREATE(allocator, tb_atlas_box);
    tb_atlas_box best = {0};
    uint64_t best_area = UINT64_MAX;
    uint32_t width = 1;
    for (size_t i = 0; i < count; i++)
    {
        width = tb_atlas_max(width, pieces[i].width - gap);
    }
    tb_sort(pieces, count, sizeof(*pieces), tb_atlas_largest_first, NULL);
    for (;;)
    {
        const tb_atlas_box bin = {0, 0, width + gap, limit.bottom + gap};
        tb_atlas_box extent = {0};
        if (tb_atlas_arrange(pieces, count, bin, spaces, next, &extent))
        {
            extent.right -= gap;
            extent.bottom -= gap;
            if (options->power_of_two)
            {
                extent.right = tb_atlas_power_of_two(extent.right);
                extent.bottom = tb_atlas_power_of_two(extent.bottom);
            }
            const uint64_t area = (uint64_t)extent.right * extent.bottom;
            if (area < best_area)
            {
                best_area = area;
                best = extent;
                for (size_t i = 0; i < count; i++)
                {
                    tb_bitmap_rect *const rect = &frames[pieces[i].index].rect;
                    rect->x = (int32_t)(pieces[i].placement.left + options->extrude);
                    rect->y = (int32_t)(pieces[i].placement.top + options->extrude);
                }
            }
        }
        if (width == limit.right)
        {
            break;
        }
        width = tb_atlas_min(width * 2u, limit.right);
    }
    tb_array_destroy(next);
    tb_array_destroy(spaces);
    return best;
}

/* Blit once with the bitmap module's format and alpha conversion; extrude
 * those converted edges, including the corners. */
static void tb_atlas_paint(tb_bitmap *atlas, const tb_sprite_frame *frame, const tb_bitmap *bitmap,
                           uint32_t border)
{
    const tb_bitmap_rect rect = frame->rect;
    const tb_bitmap_rect source = {frame->offset_x, frame->offset_y, rect.width, rect.height};
    tb_bitmap_blit(atlas, rect.x, rect.y, bitmap, &source, TB_BITMAP_BLEND_NONE);
    if (border == 0)
    {
        return;
    }
    const uint32_t x = (uint32_t)rect.x;
    const uint32_t y = (uint32_t)rect.y;
    const uint32_t right = x + rect.width;
    const uint32_t bottom = y + rect.height;
    for (uint32_t row = y; row < bottom; row++)
    {
        const tb_bitmap_color left_edge = tb_bitmap_get_pixel(atlas, x, row);
        const tb_bitmap_color right_edge = tb_bitmap_get_pixel(atlas, right - 1u, row);
        tb_bitmap_fill_rect(atlas, (tb_bitmap_rect){(int32_t)(x - border), (int32_t)row, border, 1},
                            left_edge);
        tb_bitmap_fill_rect(atlas, (tb_bitmap_rect){(int32_t)right, (int32_t)row, border, 1},
                            right_edge);
    }
    const size_t start = (size_t)(x - border) * 4u;
    const size_t bytes = (size_t)(rect.width + 2u * border) * 4u;
    for (uint32_t row = 0; row < border; row++)
    {
        memcpy(tb_bitmap_row(atlas, y - border + row) + start,
               tb_bitmap_const_row(atlas, y) + start, bytes);
        memcpy(tb_bitmap_row(atlas, bottom + row) + start,
               tb_bitmap_const_row(atlas, bottom - 1u) + start, bytes);
    }
}

tb_sprite_result tb_sprite_atlas_pack(const tb_sprite_atlas_input *inputs, size_t count,
                                      const tb_sprite_atlas_options *options,
                                      const tb_allocator *allocator, tb_sprite_sheet *sheet,
                                      tb_bitmap **atlas)
{
    const tb_sprite_atlas_options defaults = {0};
    const tb_sprite_atlas_options *const config = options != NULL ? options : &defaults;
    const tb_atlas_box limit = {0, 0, tb_atlas_limit(config->max_width, config->power_of_two),
                                tb_atlas_limit(config->max_height, config->power_of_two)};
    const uint32_t gap = tb_atlas_min(config->padding, tb_atlas_max(limit.right, limit.bottom));
    tb_sprite_result result = TB_SPRITE_OK;
    TB_ASSERT(inputs != NULL);
    TB_ASSERT(sheet != NULL);
    TB_ASSERT(atlas != NULL);
    *atlas = NULL;
    if (count == 0)
    {
        return TB_SPRITE_INVALID;
    }
    if (count > (uint64_t)limit.right * limit.bottom || count > SIZE_MAX / sizeof(tb_atlas_piece) ||
        count > SIZE_MAX / sizeof(tb_sprite_frame))
    {
        return TB_SPRITE_TOO_LARGE;
    }
    tb_atlas_piece *const pieces = TB_ALLOCATOR_NEW_ARRAY(allocator, tb_atlas_piece, count);
    tb_sprite_frame *const frames = TB_ALLOCATOR_NEW_ARRAY(allocator, tb_sprite_frame, count);
    for (size_t i = 0; i < count; i++)
    {
        const tb_sprite_atlas_input *const input = &inputs[i];
        tb_bitmap_info info = {0};
        if (input->bitmap == NULL || input->name == NULL || input->name[0] == '\0')
        {
            result = TB_SPRITE_INVALID;
            goto cleanup;
        }
        tb_bitmap_info_of(input->bitmap, &info);
        if (info.width == 0 || info.height == 0)
        {
            result = TB_SPRITE_INVALID;
            goto cleanup;
        }
        const tb_bitmap_rect crop = tb_atlas_crop(input->bitmap, &info, config->trim);
        const uint64_t width = crop.width + 2ull * config->extrude;
        const uint64_t height = crop.height + 2ull * config->extrude;
        if (width > limit.right || height > limit.bottom)
        {
            result = TB_SPRITE_TOO_LARGE;
            goto cleanup;
        }
        pieces[i] = (tb_atlas_piece){
            .index = i, .width = (uint32_t)width + gap, .height = (uint32_t)height + gap};
        frames[i] = (tb_sprite_frame){.name = input->name,
                                      .rect = crop,
                                      .offset_x = crop.x,
                                      .offset_y = crop.y,
                                      .source_width = info.width,
                                      .source_height = info.height,
                                      .pivot_x = input->pivot_x,
                                      .pivot_y = input->pivot_y};
    }
    const tb_atlas_box extent =
        tb_atlas_layout(pieces, count, frames, limit, config, gap, allocator);
    if (extent.right == 0 || (uint64_t)extent.right * extent.bottom > SIZE_MAX / 4u)
    {
        result = TB_SPRITE_TOO_LARGE;
        goto cleanup;
    }
    const size_t before = tb_sprite_sheet_frame_count(sheet);
    for (size_t i = 0; i < count; i++)
    {
        result = tb_sprite_sheet_add_frame(sheet, &frames[i], NULL);
        if (result != TB_SPRITE_OK)
        {
            tb_sprite_sheet_truncate(sheet, before, tb_sprite_sheet_animation_count(sheet));
            goto cleanup;
        }
    }
    *atlas = tb_bitmap_create(extent.right, extent.bottom, TB_BITMAP_RGBA8, allocator);
    for (size_t i = 0; i < count; i++)
    {
        tb_atlas_paint(*atlas, &frames[i], inputs[i].bitmap, config->extrude);
    }
cleanup:
    TB_ALLOCATOR_DELETE_ARRAY(allocator, tb_sprite_frame, frames, count);
    TB_ALLOCATOR_DELETE_ARRAY(allocator, tb_atlas_piece, pieces, count);
    return result;
}
