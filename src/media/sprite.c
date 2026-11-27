/* Sprite sheets: frames and animations, kept in arrays in the order they
 * were added, and found by name through tables. */
#include "sprite_internal.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/str.h"

#include <math.h>
#include <string.h>

/* Bytes of a grid frame's name past its grid's: "_" and up to ten digits. */
#define TB_SPRITE_NUMBER_TEXT 22u

typedef struct sprite_frame_record
{
    tb_sprite_frame frame;
    char *name;
} sprite_frame_record;

typedef struct sprite_animation_record
{
    tb_sprite_animation animation;
    char *name;
    tb_sprite_step *steps;
} sprite_animation_record;

const char *tb_sprite_result_name(tb_sprite_result result)
{
    switch (result)
    {
    case TB_SPRITE_OK:
        return "ok";
    case TB_SPRITE_INVALID:
        return "invalid argument";
    case TB_SPRITE_NAME_TAKEN:
        return "name taken";
    case TB_SPRITE_NOT_FOUND:
        return "not found";
    case TB_SPRITE_MALFORMED:
        return "malformed";
    case TB_SPRITE_FILE_INVALID:
        return "not a sprite sheet";
    case TB_SPRITE_UNSUPPORTED:
        return "unsupported";
    case TB_SPRITE_TOO_LARGE:
        return "too large";
    case TB_SPRITE_INPUT_FAILED:
        return "input failed";
    case TB_SPRITE_OUTPUT_FAILED:
        return "output failed";
    }
    return "unknown result";
}

tb_sprite_sheet *tb_sprite_sheet_create(const tb_allocator *allocator)
{
    tb_sprite_sheet *const sheet = TB_ALLOCATOR_NEW(allocator, tb_sprite_sheet);
    const tb_allocator resolved = tb_allocator_resolve(allocator);

    *sheet = (tb_sprite_sheet){.allocator = resolved};
    sheet->frames = TB_ARRAY_CREATE(&sheet->allocator, sprite_frame_record);
    sheet->animations = TB_ARRAY_CREATE(&sheet->allocator, sprite_animation_record);
    sheet->frame_names =
        tb_table_create(&sheet->allocator, 0, tb_table_string_hash, tb_table_string_equal);
    sheet->animation_names =
        tb_table_create(&sheet->allocator, 0, tb_table_string_hash, tb_table_string_equal);
    return sheet;
}

void tb_sprite_sheet_destroy(tb_sprite_sheet *sheet)
{
    if (sheet == NULL)
    {
        return;
    }
    const tb_allocator allocator = sheet->allocator;
    tb_sprite_sheet_truncate(sheet, 0, 0);
    tb_table_destroy(sheet->frame_names);
    tb_table_destroy(sheet->animation_names);
    tb_array_destroy(sheet->frames);
    tb_array_destroy(sheet->animations);
    tb_str_free(&allocator, sheet->image);
    TB_ALLOCATOR_DELETE(&allocator, tb_sprite_sheet, sheet);
}

void tb_sprite_sheet_truncate(tb_sprite_sheet *sheet, size_t frames, size_t animations)
{
    while (tb_array_length(sheet->animations) > animations)
    {
        const size_t last = tb_array_length(sheet->animations) - 1u;
        sprite_animation_record *const record =
            TB_ARRAY_AT(sheet->animations, sprite_animation_record, last);
        (void)tb_table_remove(sheet->animation_names, record->name, NULL);
        tb_str_free(&sheet->allocator, record->name);
        TB_ALLOCATOR_DELETE_ARRAY(&sheet->allocator, tb_sprite_step, record->steps,
                                  record->animation.step_count);
        tb_array_resize(sheet->animations, last);
    }
    while (tb_array_length(sheet->frames) > frames)
    {
        const size_t last = tb_array_length(sheet->frames) - 1u;
        sprite_frame_record *const record = TB_ARRAY_AT(sheet->frames, sprite_frame_record, last);
        (void)tb_table_remove(sheet->frame_names, record->name, NULL);
        tb_str_free(&sheet->allocator, record->name);
        tb_array_resize(sheet->frames, last);
    }
}

void tb_sprite_sheet_set_image(tb_sprite_sheet *sheet, const char *image)
{
    TB_ASSERT(sheet != NULL);
    char *const copy =
        image != NULL && image[0] != '\0' ? tb_str_dup(&sheet->allocator, image) : NULL;
    tb_str_free(&sheet->allocator, sheet->image);
    sheet->image = copy;
}

const char *tb_sprite_sheet_image(const tb_sprite_sheet *sheet)
{
    TB_ASSERT(sheet != NULL);
    return sheet->image != NULL ? sheet->image : "";
}

/* An index kept as a table's value: one more, so that 0 is never stored. */
static void *tb_sprite_index_value(size_t index)
{
    return (void *)(uintptr_t)(index + 1u);
}

static size_t tb_sprite_find(const tb_table *table, const char *name)
{
    const uintptr_t value = (uintptr_t)tb_table_get(table, name);
    return value == 0 ? TB_SPRITE_NONE : (size_t)(value - 1u);
}

/* Whether a frame's rectangle and trimming describe pixels. */
static bool tb_sprite_frame_valid(const tb_sprite_frame *frame)
{
    const uint32_t source_width =
        frame->source_width != 0 ? frame->source_width : frame->rect.width;
    const uint32_t source_height =
        frame->source_height != 0 ? frame->source_height : frame->rect.height;

    return frame->name != NULL && frame->name[0] != '\0' && frame->rect.width > 0 &&
           frame->rect.height > 0 && frame->rect.x >= 0 && frame->rect.y >= 0 &&
           frame->rect.width <= TB_BITMAP_MAX_DIMENSION &&
           frame->rect.height <= TB_BITMAP_MAX_DIMENSION &&
           source_width <= TB_BITMAP_MAX_DIMENSION && source_height <= TB_BITMAP_MAX_DIMENSION &&
           frame->offset_x >= 0 && frame->offset_y >= 0 &&
           (uint64_t)(uint32_t)frame->offset_x + frame->rect.width <= source_width &&
           (uint64_t)(uint32_t)frame->offset_y + frame->rect.height <= source_height &&
           (uint64_t)(uint32_t)frame->rect.x + frame->rect.width <= TB_BITMAP_MAX_DIMENSION &&
           (uint64_t)(uint32_t)frame->rect.y + frame->rect.height <= TB_BITMAP_MAX_DIMENSION &&
           isfinite(frame->pivot_x) && isfinite(frame->pivot_y);
}

tb_sprite_result tb_sprite_sheet_add_frame(tb_sprite_sheet *sheet, const tb_sprite_frame *frame,
                                           size_t *index)
{
    TB_ASSERT(sheet != NULL);
    TB_ASSERT(frame != NULL);
    if (!tb_sprite_frame_valid(frame))
    {
        return TB_SPRITE_INVALID;
    }
    if (tb_sprite_find(sheet->frame_names, frame->name) != TB_SPRITE_NONE)
    {
        return TB_SPRITE_NAME_TAKEN;
    }
    tb_sprite_frame copy = *frame;
    char *const name = tb_str_dup(&sheet->allocator, frame->name);
    copy.name = name;
    if (copy.source_width == 0)
    {
        copy.source_width = copy.rect.width;
    }
    if (copy.source_height == 0)
    {
        copy.source_height = copy.rect.height;
    }
    const size_t added = tb_array_length(sheet->frames);
    const sprite_frame_record record = {.frame = copy, .name = name};
    TB_ARRAY_PUSH(sheet->frames, sprite_frame_record, record);
    (void)tb_table_put(sheet->frame_names, copy.name, tb_sprite_index_value(added));
    if (index != NULL)
    {
        *index = added;
    }
    return TB_SPRITE_OK;
}

/* Columns or rows of a grid that fit in `size` pixels. */
static uint32_t tb_sprite_cells_that_fit(uint32_t size, uint32_t margin, uint32_t cell,
                                         uint32_t spacing)
{
    if (size < margin || size - margin < cell)
    {
        return 0;
    }
    return (uint32_t)(1u + ((uint64_t)size - margin - cell) / ((uint64_t)cell + spacing));
}

tb_sprite_result tb_sprite_sheet_add_grid(tb_sprite_sheet *sheet, const tb_sprite_grid *grid,
                                          size_t *first, size_t *count)
{
    TB_ASSERT(sheet != NULL);
    TB_ASSERT(grid != NULL);
    if (grid->cell_width == 0 || grid->cell_height == 0 || grid->name == NULL ||
        grid->name[0] == '\0')
    {
        return TB_SPRITE_INVALID;
    }
    const size_t before = tb_array_length(sheet->frames);
    const uint32_t columns = grid->columns != 0
                                 ? grid->columns
                                 : tb_sprite_cells_that_fit(grid->image_width, grid->margin_x,
                                                            grid->cell_width, grid->spacing_x);
    const uint32_t rows = grid->rows != 0
                              ? grid->rows
                              : tb_sprite_cells_that_fit(grid->image_height, grid->margin_y,
                                                         grid->cell_height, grid->spacing_y);
    const uint64_t cells = (uint64_t)columns * rows;
    const uint64_t wanted = grid->count != 0 ? grid->count : cells;
    const size_t prefix = tb_str_length(grid->name);
    if (cells == 0 || wanted > cells || wanted > SIZE_MAX ||
        prefix > SIZE_MAX - TB_SPRITE_NUMBER_TEXT)
    {
        return TB_SPRITE_INVALID;
    }
    const uint64_t last_column = wanted < columns ? wanted - 1u : columns - 1u;
    const uint64_t last_row = (wanted - 1u) / columns;
    const uint64_t x_stride = (uint64_t)grid->cell_width + grid->spacing_x;
    const uint64_t y_stride = (uint64_t)grid->cell_height + grid->spacing_y;
    const uint64_t first_right = (uint64_t)grid->margin_x + grid->cell_width;
    const uint64_t first_bottom = (uint64_t)grid->margin_y + grid->cell_height;
    const uint32_t max_x = grid->image_width && grid->image_width < TB_BITMAP_MAX_DIMENSION
                               ? grid->image_width
                               : TB_BITMAP_MAX_DIMENSION;
    const uint32_t max_y = grid->image_height && grid->image_height < TB_BITMAP_MAX_DIMENSION
                               ? grid->image_height
                               : TB_BITMAP_MAX_DIMENSION;
    if (first_right > max_x || first_bottom > max_y ||
        last_column > (max_x - first_right) / x_stride ||
        last_row > (max_y - first_bottom) / y_stride)
    {
        return TB_SPRITE_INVALID;
    }
    const size_t capacity = prefix + TB_SPRITE_NUMBER_TEXT;
    char *const name = TB_ALLOCATOR_NEW_ARRAY(&sheet->allocator, char, capacity);
    tb_sprite_result result = TB_SPRITE_OK;
    for (uint64_t cell = 0; cell < wanted; cell++)
    {
        const uint32_t column = (uint32_t)(cell % columns);
        const uint32_t row = (uint32_t)(cell / columns);
        (void)tb_fmt_buffer(name, capacity, "%s_%llu", grid->name, (unsigned long long)cell);
        const tb_sprite_frame frame = {
            .name = name,
            .rect = {(int32_t)(grid->margin_x + column * (grid->cell_width + grid->spacing_x)),
                     (int32_t)(grid->margin_y + row * (grid->cell_height + grid->spacing_y)),
                     grid->cell_width, grid->cell_height}};
        result = tb_sprite_sheet_add_frame(sheet, &frame, NULL);
        if (result != TB_SPRITE_OK)
        {
            tb_sprite_sheet_truncate(sheet, before, tb_array_length(sheet->animations));
            break;
        }
    }
    TB_ALLOCATOR_DELETE_ARRAY(&sheet->allocator, char, name, capacity);
    if (result != TB_SPRITE_OK)
    {
        return result;
    }
    if (first != NULL)
    {
        *first = before;
    }
    if (count != NULL)
    {
        *count = (size_t)wanted;
    }
    return TB_SPRITE_OK;
}

tb_sprite_result tb_sprite_sheet_add_animation(tb_sprite_sheet *sheet, const char *name,
                                               const tb_sprite_step *steps, size_t count,
                                               tb_sprite_loop loop, size_t *index)
{
    uint64_t duration = 0;

    TB_ASSERT(sheet != NULL);
    TB_ASSERT(name != NULL);
    TB_ASSERT(steps != NULL || count == 0);
    if (name[0] == '\0' || count == 0 || (unsigned)loop >= (unsigned)TB_SPRITE_LOOP_COUNT)
    {
        return TB_SPRITE_INVALID;
    }
    for (size_t step = 0; step < count; step++)
    {
        if (steps[step].frame >= tb_array_length(sheet->frames) || steps[step].duration_ms == 0 ||
            UINT64_MAX - duration < steps[step].duration_ms)
        {
            return TB_SPRITE_INVALID;
        }
        duration += steps[step].duration_ms;
    }
    if (loop == TB_SPRITE_PING_PONG && count > 1 &&
        UINT64_MAX - duration < duration - steps[0].duration_ms - steps[count - 1u].duration_ms)
    {
        return TB_SPRITE_INVALID;
    }
    if (tb_sprite_find(sheet->animation_names, name) != TB_SPRITE_NONE)
    {
        return TB_SPRITE_NAME_TAKEN;
    }
    tb_sprite_step *const copy = TB_ALLOCATOR_NEW_ARRAY(&sheet->allocator, tb_sprite_step, count);
    memcpy(copy, steps, count * sizeof(*copy));
    char *const owned_name = tb_str_dup(&sheet->allocator, name);
    const tb_sprite_animation animation = {.name = owned_name,
                                           .steps = copy,
                                           .step_count = count,
                                           .loop = loop,
                                           .duration_ms = duration};
    const size_t added = tb_array_length(sheet->animations);
    const sprite_animation_record record = {
        .animation = animation, .name = owned_name, .steps = copy};
    TB_ARRAY_PUSH(sheet->animations, sprite_animation_record, record);
    (void)tb_table_put(sheet->animation_names, animation.name, tb_sprite_index_value(added));
    if (index != NULL)
    {
        *index = added;
    }
    return TB_SPRITE_OK;
}

size_t tb_sprite_sheet_frame_count(const tb_sprite_sheet *sheet)
{
    TB_ASSERT(sheet != NULL);
    return tb_array_length(sheet->frames);
}

const tb_sprite_frame *tb_sprite_sheet_frame(const tb_sprite_sheet *sheet, size_t index)
{
    TB_ASSERT(sheet != NULL);
    TB_ASSERT_MSG(index < tb_array_length(sheet->frames), "a frame of the sheet");
    return &TB_ARRAY_AT(sheet->frames, sprite_frame_record, index)->frame;
}

size_t tb_sprite_sheet_find_frame(const tb_sprite_sheet *sheet, const char *name)
{
    TB_ASSERT(sheet != NULL);
    TB_ASSERT(name != NULL);
    return tb_sprite_find(sheet->frame_names, name);
}

size_t tb_sprite_sheet_animation_count(const tb_sprite_sheet *sheet)
{
    TB_ASSERT(sheet != NULL);
    return tb_array_length(sheet->animations);
}

const tb_sprite_animation *tb_sprite_sheet_animation(const tb_sprite_sheet *sheet, size_t index)
{
    TB_ASSERT(sheet != NULL);
    TB_ASSERT_MSG(index < tb_array_length(sheet->animations), "an animation of the sheet");
    return &TB_ARRAY_AT(sheet->animations, sprite_animation_record, index)->animation;
}

size_t tb_sprite_sheet_find_animation(const tb_sprite_sheet *sheet, const char *name)
{
    TB_ASSERT(sheet != NULL);
    TB_ASSERT(name != NULL);
    return tb_sprite_find(sheet->animation_names, name);
}

/* The step at a time within one pass forward through the steps. */
static size_t tb_sprite_step_within(const tb_sprite_animation *animation, uint64_t time_ms)
{
    uint64_t elapsed = 0;

    for (size_t step = 0; step < animation->step_count; step++)
    {
        elapsed += animation->steps[step].duration_ms;
        if (time_ms < elapsed)
        {
            return step;
        }
    }
    return animation->step_count - 1u;
}

size_t tb_sprite_animation_step_at(const tb_sprite_animation *animation, uint64_t time_ms)
{
    TB_ASSERT(animation != NULL);
    TB_ASSERT(animation->step_count > 0);
    const uint64_t once = animation->duration_ms;

    switch (animation->loop)
    {
    case TB_SPRITE_ONCE:
        return tb_sprite_step_within(animation, time_ms);
    case TB_SPRITE_PING_PONG: {
        if (animation->step_count == 1u)
        {
            return 0;
        }
        /* Forward through every step, then back through those between the
         * ends: a cycle of the whole, less the two ends' durations. */
        const uint64_t ends = (uint64_t)animation->steps[0].duration_ms +
                              animation->steps[animation->step_count - 1u].duration_ms;
        const uint64_t cycle = once + (once - ends);
        const uint64_t within = time_ms % cycle;
        if (within < once)
        {
            return tb_sprite_step_within(animation, within);
        }
        /* Back: the steps from the second last to the second, in reverse. */
        uint64_t back = within - once;
        for (size_t step = animation->step_count - 2u; step > 0; step--)
        {
            if (back < animation->steps[step].duration_ms)
            {
                return step;
            }
            back -= animation->steps[step].duration_ms;
        }
        return 1u; /* coverage: unreachable for a cycle computed as above */
    }
    case TB_SPRITE_LOOP:
    case TB_SPRITE_LOOP_COUNT:
    default:
        return tb_sprite_step_within(animation, time_ms % once);
    }
}
