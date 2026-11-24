/* Glyph outlines: TrueType's simple glyphs of on- and off-curve points, and
 * compound glyphs of other glyphs placed and transformed, made into contours
 * of explicit lines and quadratic curves. */
#include "badger/font.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "font_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TB_FONT_GLYPH_HEADER ((size_t)10)

/* Flags of a simple glyph's points. */
#define TB_FONT_POINT_ON_CURVE 0x01u
#define TB_FONT_POINT_X_SHORT 0x02u
#define TB_FONT_POINT_Y_SHORT 0x04u
#define TB_FONT_POINT_REPEAT 0x08u
#define TB_FONT_POINT_X_SAME_OR_POSITIVE 0x10u
#define TB_FONT_POINT_Y_SAME_OR_POSITIVE 0x20u

/* Flags of a compound glyph's components. */
#define TB_FONT_COMPONENT_WORDS 0x0001u
#define TB_FONT_COMPONENT_XY 0x0002u
#define TB_FONT_COMPONENT_SCALE 0x0008u
#define TB_FONT_COMPONENT_MORE 0x0020u
#define TB_FONT_COMPONENT_XY_SCALE 0x0040u
#define TB_FONT_COMPONENT_TWO_BY_TWO 0x0080u
#define TB_FONT_COMPONENT_SCALED_OFFSET 0x0800u
#define TB_FONT_COMPONENT_UNSCALED_OFFSET 0x1000u

/* F2DOT14: a 16-bit fraction with 14 bits after the point. */
#define TB_FONT_F2DOT14_ONE 16384.0f

/* Components one outline may visit: a bound on the work of compounds that
 * nest many components of nothing. */
#define TB_FONT_MAX_COMPONENTS ((size_t)1 << 20)

/* One component of a compound glyph. */
typedef struct tb_font_component
{
    uint16_t flags;
    uint16_t glyph;
    int32_t first;  /* An x offset, or the compound's point to match. */
    int32_t second; /* A y offset, or the component's point to match. */
    float xx;       /* The transform: x' = xx x + yx y, y' = xy x + yy y. */
    float xy;
    float yx;
    float yy;
    size_t length; /* Bytes of the component's record. */
} tb_font_component;

/* An outline as it is built: first counted, then, with the arrays
 * allocated, filled with points. */
typedef struct tb_font_builder
{
    const tb_font *font;
    tb_font_error *error;
    bool filling;
    size_t point_count;
    size_t contour_count;
    size_t component_count;
    tb_font_point *points;
    bool *on_curve;
    size_t *contour_ends; /* Index of the last point of each contour. */
    size_t point_capacity;
    size_t contour_capacity;
} tb_font_builder;

static tb_font_result tb_font_glyph_fail(tb_font_builder *builder, tb_font_result result,
                                         const char *reason)
{
    return tb_font_fail(builder->error, result, "glyf", reason);
}

/* -------------------------------------------------------------------------
 * Simple glyphs.
 * ------------------------------------------------------------------------- */

/* Read the flags of `count` points from `*offset` on, into `flags`. */
static tb_font_result tb_font_read_flags(tb_font_builder *builder, tb_font_span data,
                                         size_t *offset, uint8_t *flags, size_t count)
{
    size_t index = 0;
    while (index < count)
    {
        if (*offset >= data.length)
        {
            return tb_font_glyph_fail(builder, TB_FONT_MALFORMED, "the point flags are cut off");
        }
        const uint8_t flag = data.bytes[*offset];
        *offset += 1;
        size_t repeat = 1;
        if ((flag & TB_FONT_POINT_REPEAT) != 0)
        {
            if (*offset >= data.length)
            {
                return tb_font_glyph_fail(builder, TB_FONT_MALFORMED,
                                          "the point flags are cut off");
            }
            repeat += data.bytes[*offset];
            *offset += 1;
        }
        if (repeat > count - index)
        {
            return tb_font_glyph_fail(builder, TB_FONT_MALFORMED,
                                      "the point flags repeat past the last point");
        }
        for (size_t copy = 0; copy < repeat; copy++)
        {
            flags[index] = flag;
            index++;
        }
    }
    return TB_FONT_OK;
}

/* Read one axis of coordinates, each a change from the one before, into
 * `values`. `short_flag` marks a one-byte change, whose sign `same_flag`
 * gives; without it, `same_flag` marks no change, and its absence a
 * two-byte one. */
static tb_font_result tb_font_read_axis(tb_font_builder *builder, tb_font_span data, size_t *offset,
                                        const uint8_t *flags, size_t count, unsigned short_flag,
                                        unsigned same_flag, int32_t *values)
{
    int32_t value = 0;
    for (size_t index = 0; index < count; index++)
    {
        const unsigned flag = flags[index];
        if ((flag & short_flag) != 0)
        {
            if (*offset >= data.length)
            {
                return tb_font_glyph_fail(builder, TB_FONT_MALFORMED,
                                          "the coordinates are cut off");
            }
            const int32_t change = data.bytes[*offset];
            *offset += 1;
            value += (flag & same_flag) != 0 ? change : -change;
        }
        else if ((flag & same_flag) == 0)
        {
            if (!tb_font_fits(*offset, 2, data.length))
            {
                return tb_font_glyph_fail(builder, TB_FONT_MALFORMED,
                                          "the coordinates are cut off");
            }
            value += tb_font_s16(data.bytes + *offset);
            *offset += 2;
        }
        /* No more than 65536 changes of 16 bits each: the sum fits 32 bits. */
        values[index] = value;
    }
    return TB_FONT_OK;
}

/* The points of a simple glyph of `contours` contours, `point_count` points in all. */
static tb_font_result tb_font_fill_simple(tb_font_builder *builder, tb_font_span data,
                                          size_t contours, size_t point_count, tb_arena *scratch)
{
    const size_t base = builder->point_count;
    TB_ASSERT(point_count <= builder->point_capacity - base);
    TB_ASSERT(contours <= builder->contour_capacity - builder->contour_count);
    size_t previous = 0;
    for (size_t contour = 0; contour < contours; contour++)
    {
        const size_t end = tb_font_u16(data.bytes + TB_FONT_GLYPH_HEADER + contour * 2);
        if (contour > 0 && end <= previous)
        {
            return tb_font_glyph_fail(builder, TB_FONT_MALFORMED, "contour ends are out of order");
        }
        previous = end;
        builder->contour_ends[builder->contour_count + contour] = base + end;
    }
    size_t offset = TB_FONT_GLYPH_HEADER + contours * 2;
    if (!tb_font_fits(offset, 2, data.length))
    {
        return tb_font_glyph_fail(builder, TB_FONT_MALFORMED, "the instructions are cut off");
    }
    offset += 2 + (size_t)tb_font_u16(data.bytes + offset);
    if (offset > data.length)
    {
        return tb_font_glyph_fail(builder, TB_FONT_MALFORMED, "the instructions are cut off");
    }
    const tb_arena_mark mark = tb_arena_get_mark(scratch);
    uint8_t *flags = TB_ARENA_NEW_ARRAY(scratch, uint8_t, point_count);
    int32_t *xs = TB_ARENA_NEW_ARRAY(scratch, int32_t, point_count);
    int32_t *ys = TB_ARENA_NEW_ARRAY(scratch, int32_t, point_count);
    tb_font_result result = tb_font_read_flags(builder, data, &offset, flags, point_count);
    if (result != TB_FONT_OK)
    {
        goto done;
    }
    result = tb_font_read_axis(builder, data, &offset, flags, point_count, TB_FONT_POINT_X_SHORT,
                               TB_FONT_POINT_X_SAME_OR_POSITIVE, xs);
    if (result != TB_FONT_OK)
    {
        goto done;
    }
    result = tb_font_read_axis(builder, data, &offset, flags, point_count, TB_FONT_POINT_Y_SHORT,
                               TB_FONT_POINT_Y_SAME_OR_POSITIVE, ys);
    if (result != TB_FONT_OK)
    {
        goto done;
    }
    for (size_t index = 0; index < point_count; index++)
    {
        builder->points[base + index].x = (float)xs[index];
        builder->points[base + index].y = (float)ys[index];
        builder->on_curve[base + index] = (flags[index] & TB_FONT_POINT_ON_CURVE) != 0;
    }
done:
    tb_arena_rewind(scratch, mark);
    return result;
}

/* Count a simple glyph's points and contours, and, when filling, read them. */
static tb_font_result tb_font_load_simple(tb_font_builder *builder, tb_font_span data,
                                          size_t contours, tb_arena *scratch)
{
    if (!tb_font_array_fits(TB_FONT_GLYPH_HEADER, contours, 2, data.length))
    {
        return tb_font_glyph_fail(builder, TB_FONT_MALFORMED, "the contour ends are cut off");
    }
    const size_t point_count =
        contours == 0
            ? 0
            : (size_t)tb_font_u16(data.bytes + TB_FONT_GLYPH_HEADER + (contours - 1) * 2) + 1;
    if (point_count > TB_FONT_MAX_OUTLINE_POINTS - builder->point_count ||
        contours > TB_FONT_MAX_OUTLINE_POINTS - builder->contour_count)
    {
        return tb_font_glyph_fail(builder, TB_FONT_TOO_LARGE, "the outline has too many points");
    }
    if (builder->filling && contours > 0)
    {
        const tb_font_result result =
            tb_font_fill_simple(builder, data, contours, point_count, scratch);
        if (result != TB_FONT_OK)
        {
            return result;
        }
    }
    builder->point_count += point_count;
    builder->contour_count += contours;
    return TB_FONT_OK;
}

/* -------------------------------------------------------------------------
 * Compound glyphs.
 * ------------------------------------------------------------------------- */

static float tb_font_f2dot14(const uint8_t *bytes)
{
    return (float)tb_font_s16(bytes) / TB_FONT_F2DOT14_ONE;
}

/* The size of a component's transform, by its flags. */
static size_t tb_font_transform_size(uint16_t flags)
{
    if ((flags & TB_FONT_COMPONENT_SCALE) != 0)
    {
        return 2;
    }
    if ((flags & TB_FONT_COMPONENT_XY_SCALE) != 0)
    {
        return 4;
    }
    return (flags & TB_FONT_COMPONENT_TWO_BY_TWO) != 0 ? 8 : 0;
}

static void tb_font_read_arguments(const uint8_t *bytes, tb_font_component *component)
{
    const bool words = (component->flags & TB_FONT_COMPONENT_WORDS) != 0;
    const bool offsets = (component->flags & TB_FONT_COMPONENT_XY) != 0;
    if (words && offsets)
    {
        component->first = tb_font_s16(bytes);
        component->second = tb_font_s16(bytes + 2);
    }
    else if (words)
    {
        component->first = tb_font_u16(bytes);
        component->second = tb_font_u16(bytes + 2);
    }
    else if (offsets)
    {
        component->first = bytes[0] >= 0x80 ? (int32_t)bytes[0] - 0x100 : bytes[0];
        component->second = bytes[1] >= 0x80 ? (int32_t)bytes[1] - 0x100 : bytes[1];
    }
    else
    {
        component->first = bytes[0];
        component->second = bytes[1];
    }
}

static void tb_font_read_transform(const uint8_t *bytes, tb_font_component *component)
{
    component->xx = 1.0f;
    component->xy = 0.0f;
    component->yx = 0.0f;
    component->yy = 1.0f;
    if ((component->flags & TB_FONT_COMPONENT_SCALE) != 0)
    {
        component->xx = tb_font_f2dot14(bytes);
        component->yy = component->xx;
    }
    else if ((component->flags & TB_FONT_COMPONENT_XY_SCALE) != 0)
    {
        component->xx = tb_font_f2dot14(bytes);
        component->yy = tb_font_f2dot14(bytes + 2);
    }
    else if ((component->flags & TB_FONT_COMPONENT_TWO_BY_TWO) != 0)
    {
        component->xx = tb_font_f2dot14(bytes);
        component->xy = tb_font_f2dot14(bytes + 2);
        component->yx = tb_font_f2dot14(bytes + 4);
        component->yy = tb_font_f2dot14(bytes + 6);
    }
}

/* Read the component whose record begins at `offset`. */
static tb_font_result tb_font_read_component(tb_font_builder *builder, tb_font_span data,
                                             size_t offset, tb_font_component *component)
{
    if (!tb_font_fits(offset, 4, data.length))
    {
        return tb_font_glyph_fail(builder, TB_FONT_MALFORMED, "a component is cut off");
    }
    component->flags = tb_font_u16(data.bytes + offset);
    component->glyph = tb_font_u16(data.bytes + offset + 2);
    const size_t arguments = (component->flags & TB_FONT_COMPONENT_WORDS) != 0 ? 4 : 2;
    const size_t transform = tb_font_transform_size(component->flags);
    component->length = 4 + arguments + transform;
    if (!tb_font_fits(offset, component->length, data.length))
    {
        return tb_font_glyph_fail(builder, TB_FONT_MALFORMED, "a component is cut off");
    }
    if (component->glyph >= builder->font->metrics.glyph_count)
    {
        return tb_font_glyph_fail(builder, TB_FONT_MALFORMED,
                                  "a component is a glyph the font does not have");
    }
    tb_font_read_arguments(data.bytes + offset + 4, component);
    tb_font_read_transform(data.bytes + offset + 4 + arguments, component);
    return TB_FONT_OK;
}

/* Transform and place the points a component added, from `first` on; `base`
 * is where the compound's own points begin. */
static tb_font_result tb_font_place_component(tb_font_builder *builder,
                                              const tb_font_component *component, size_t base,
                                              size_t first)
{
    tb_font_point *points = builder->points;
    const size_t end = builder->point_count;
    for (size_t index = first; index < end; index++)
    {
        const tb_font_point point = points[index];
        points[index].x = component->xx * point.x + component->yx * point.y;
        points[index].y = component->xy * point.x + component->yy * point.y;
    }
    float dx = 0.0f;
    float dy = 0.0f;
    if ((component->flags & TB_FONT_COMPONENT_XY) != 0)
    {
        dx = (float)component->first;
        dy = (float)component->second;
        const bool scaled = (component->flags & (TB_FONT_COMPONENT_SCALED_OFFSET |
                                                 TB_FONT_COMPONENT_UNSCALED_OFFSET)) ==
                            TB_FONT_COMPONENT_SCALED_OFFSET;
        if (scaled)
        {
            const float x = dx;
            dx = component->xx * x + component->yx * dy;
            dy = component->xy * x + component->yy * dy;
        }
    }
    else
    {
        /* Move the component so that its point lands on the compound's. */
        const size_t compound_point = (size_t)component->first;
        const size_t component_point = (size_t)component->second;
        if (compound_point >= first - base || component_point >= end - first)
        {
            return tb_font_glyph_fail(builder, TB_FONT_MALFORMED,
                                      "a component matches a point that does not exist");
        }
        dx = points[base + compound_point].x - points[first + component_point].x;
        dy = points[base + compound_point].y - points[first + component_point].y;
    }
    for (size_t index = first; index < end; index++)
    {
        points[index].x += dx;
        points[index].y += dy;
    }
    return TB_FONT_OK;
}

static tb_font_result tb_font_load_glyph(tb_font_builder *builder, uint16_t glyph, int depth,
                                         tb_arena *scratch);

static tb_font_result tb_font_load_compound(tb_font_builder *builder, tb_font_span data, int depth,
                                            tb_arena *scratch)
{
    if (depth >= TB_FONT_MAX_COMPOUND_DEPTH)
    {
        return tb_font_glyph_fail(builder, TB_FONT_MALFORMED,
                                  "compound glyphs nest too deeply, or in a cycle");
    }
    const size_t base = builder->point_count;
    size_t offset = TB_FONT_GLYPH_HEADER;
    bool more = true;
    while (more)
    {
        tb_font_component component = {0};
        tb_font_result result = tb_font_read_component(builder, data, offset, &component);
        if (result != TB_FONT_OK)
        {
            return result;
        }
        builder->component_count++;
        if (builder->component_count > TB_FONT_MAX_COMPONENTS)
        {
            return tb_font_glyph_fail(builder, TB_FONT_TOO_LARGE,
                                      "the outline has too many components");
        }
        const size_t first = builder->point_count;
        result = tb_font_load_glyph(builder, component.glyph, depth + 1, scratch);
        if (result == TB_FONT_OK && builder->filling)
        {
            result = tb_font_place_component(builder, &component, base, first);
        }
        if (result != TB_FONT_OK)
        {
            return result;
        }
        offset += component.length;
        more = (component.flags & TB_FONT_COMPONENT_MORE) != 0;
    }
    return TB_FONT_OK;
}

/* Count, or fill, the points and contours of a glyph at a depth of compounds. */
static tb_font_result tb_font_load_glyph(tb_font_builder *builder, uint16_t glyph, int depth,
                                         tb_arena *scratch)
{
    const tb_font_span data = tb_font_glyph_data(builder->font, glyph);
    if (data.length == 0)
    {
        return TB_FONT_OK;
    }
    if (data.length < TB_FONT_GLYPH_HEADER)
    {
        return tb_font_glyph_fail(builder, TB_FONT_MALFORMED, "the glyph header is cut off");
    }
    const int16_t contours = tb_font_s16(data.bytes);
    if (contours >= 0)
    {
        return tb_font_load_simple(builder, data, (size_t)contours, scratch);
    }
    return tb_font_load_compound(builder, data, depth, scratch);
}

/* -------------------------------------------------------------------------
 * Contours of segments.
 * ------------------------------------------------------------------------- */

static tb_font_point tb_font_midpoint(tb_font_point from, tb_font_point to)
{
    const tb_font_point middle = {(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f};
    return middle;
}

static tb_font_segment tb_font_segment_of(tb_font_segment_type type, tb_font_point control,
                                          tb_font_point end)
{
    const tb_font_segment segment = {type, control, end};
    return segment;
}

/* Make the points `first` to `last` into a contour, its segments from `segments`. */
static void tb_font_make_contour(const tb_font_builder *builder, size_t first, size_t last,
                                 tb_font_segment *segments, tb_font_contour *contour)
{
    const tb_font_point *points = builder->points;
    const size_t count = last - first + 1;
    size_t start = count;
    for (size_t index = 0; index < count; index++)
    {
        if (builder->on_curve[first + index])
        {
            start = index;
            break;
        }
    }
    size_t made = 0;
    if (start == count)
    {
        /* Only off-curve points: each curve ends between two of them. */
        contour->start = tb_font_midpoint(points[last], points[first]);
        for (size_t index = 0; index < count; index++)
        {
            const tb_font_point control = points[first + index];
            const tb_font_point next = points[first + (index + 1) % count];
            segments[made] =
                tb_font_segment_of(TB_FONT_QUADRATIC, control, tb_font_midpoint(control, next));
            made++;
        }
    }
    else
    {
        contour->start = points[first + start];
        bool pending = false;
        tb_font_point control = contour->start;
        /* Round from the first on-curve point back to it. */
        for (size_t step = 1; step <= count; step++)
        {
            const size_t index = first + (start + step) % count;
            const tb_font_point point = points[index];
            if (builder->on_curve[index])
            {
                segments[made] = pending ? tb_font_segment_of(TB_FONT_QUADRATIC, control, point)
                                         : tb_font_segment_of(TB_FONT_LINE, point, point);
                made++;
                pending = false;
            }
            else if (pending)
            {
                /* Two off-curve points in a row imply an on-curve point between them. */
                const tb_font_point middle = tb_font_midpoint(control, point);
                segments[made] = tb_font_segment_of(TB_FONT_QUADRATIC, control, middle);
                made++;
                control = point;
            }
            else
            {
                pending = true;
                control = point;
            }
        }
    }
    contour->segments = segments;
    contour->segment_count = made;
}

/* Make the builder's points into the outline's contours, in arrays allocated for them. */
static void tb_font_make_contours(const tb_font_builder *builder, tb_font_contour *contours,
                                  tb_font_segment *segments, tb_font_outline *outline)
{
    size_t first = 0;
    size_t used = 0;
    for (size_t index = 0; index < builder->contour_count; index++)
    {
        const size_t last = builder->contour_ends[index];
        tb_font_make_contour(builder, first, last, segments + used, &contours[index]);
        used += contours[index].segment_count;
        first = last + 1;
    }
    outline->contours = contours;
    outline->contour_count = builder->contour_count;
}

/* -------------------------------------------------------------------------
 * Outlines.
 * ------------------------------------------------------------------------- */

/* Build the outline in the arena: count the points and contours, allocate
 * what the outline keeps, then fill the points into scratch memory released
 * once the contours are made. */
static tb_font_result tb_font_build_outline(const tb_font *font, uint16_t glyph, tb_arena *arena,
                                            tb_font_outline *outline, tb_font_error *error)
{
    tb_font_builder builder = {0};
    builder.font = font;
    builder.error = error;
    tb_font_result result = tb_font_load_glyph(&builder, glyph, 0, arena);
    if (result != TB_FONT_OK || builder.contour_count == 0)
    {
        return result;
    }
    const size_t point_count = builder.point_count;
    const size_t contour_count = builder.contour_count;
    /* Each point ends or controls at most one segment. */
    tb_font_contour *contours = TB_ARENA_NEW_ARRAY(arena, tb_font_contour, contour_count);
    tb_font_segment *segments = TB_ARENA_NEW_ARRAY(arena, tb_font_segment, point_count);
    const tb_arena_mark mark = tb_arena_get_mark(arena);
    builder.filling = true;
    builder.point_capacity = point_count;
    builder.contour_capacity = contour_count;
    builder.point_count = 0;
    builder.contour_count = 0;
    builder.component_count = 0;
    builder.points = TB_ARENA_NEW_ARRAY(arena, tb_font_point, point_count);
    builder.on_curve = TB_ARENA_NEW_ARRAY(arena, bool, point_count);
    builder.contour_ends = TB_ARENA_NEW_ARRAY(arena, size_t, contour_count);
    result = tb_font_load_glyph(&builder, glyph, 0, arena);
    if (result == TB_FONT_OK)
    {
        TB_ASSERT(builder.point_count == point_count && builder.contour_count == contour_count);
        tb_font_make_contours(&builder, contours, segments, outline);
    }
    tb_arena_rewind(arena, mark);
    return result;
}

/* The glyph's box, as its header records it; a glyph without data has none. */
static void tb_font_read_box(const tb_font *font, uint16_t glyph, tb_font_outline *outline)
{
    const tb_font_span data = tb_font_glyph_data(font, glyph);
    if (data.length == 0)
    {
        return;
    }
    outline->x_min = tb_font_s16(data.bytes + 2);
    outline->y_min = tb_font_s16(data.bytes + 4);
    outline->x_max = tb_font_s16(data.bytes + 6);
    outline->y_max = tb_font_s16(data.bytes + 8);
}

tb_font_result tb_font_get_outline(const tb_font *font, uint16_t glyph, tb_arena *arena,
                                   tb_font_outline *outline, tb_font_error *error)
{
    TB_ASSERT(font != NULL);
    TB_ASSERT(arena != NULL);
    TB_ASSERT(outline != NULL);
    TB_ASSERT(glyph < font->metrics.glyph_count);
    const tb_font_outline empty = {NULL, 0, 0, 0, 0, 0};
    *outline = empty;
    tb_font_error local_error = {TB_FONT_OK, "", ""};
    const tb_arena_mark mark = tb_arena_get_mark(arena);
    const tb_font_result result = tb_font_build_outline(font, glyph, arena, outline, &local_error);
    if (result != TB_FONT_OK)
    {
        tb_arena_rewind(arena, mark);
        *outline = empty;
    }
    else
    {
        tb_font_read_box(font, glyph, outline);
    }
    if (error != NULL)
    {
        *error = local_error;
    }
    return result;
}
