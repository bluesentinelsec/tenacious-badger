/* Text laid out in lines: characters read from UTF-8, placed with their
 * advances and kerning, broken at a width, aligned, and measured; and the
 * places of a caret between them. */
#include "badger/font_layout.h"

#include "badger/assert.h"
#include "badger/utf8.h"

#include <math.h>

/* What a byte that begins no character reads as. */
#define TB_FONT_REPLACEMENT_CHARACTER 0xFFFDu
/* A tab is as wide as this many spaces. */
#define TB_FONT_TAB_SPACES 4.0f
/* A line may run past its width by this much of a pixel, so that rounding
 * does not break a line that fits. */
#define TB_FONT_WRAP_SLACK 0.001f
/* The glyph before the first of a line: none, so nothing kerns against it. */
#define TB_FONT_NO_GLYPH UINT32_MAX

/* What laying out a text needs, worked out once. */
typedef struct tb_font_setup
{
    const tb_font *font;
    tb_text text;
    float pixels_per_em;
    float scale;      /* pixels a font unit */
    float wrap_width; /* 0 for none */
    bool whole_pixels;
    uint16_t space;    /* the space's glyph, which tabs draw */
    float tab_advance; /* in pixels */
} tb_font_setup;

/* One character read from the text. */
typedef struct tb_font_character
{
    uint32_t code_point;
    size_t offset;
    size_t end;
    uint16_t glyph;
    float advance; /* in pixels */
    bool newline;  /* \n: ends a line */
    bool ignored;  /* \r: takes no place */
    bool space;    /* a space or a tab: lines break after them */
    bool tab;
} tb_font_character;

/* One line as it was scanned. */
typedef struct tb_font_line_scan
{
    size_t end;  /* the line's characters end here */
    size_t next; /* the next line begins here */
    float width; /* without the spaces that end it */
    size_t glyph_count;
    bool last; /* the text ends with it */
} tb_font_line_scan;

static float tb_font_pixels(const tb_font_setup *setup, float pixels)
{
    return setup->whole_pixels ? roundf(pixels) : pixels;
}

static void tb_font_setup_of(const tb_font *font, tb_text text, float pixels_per_em,
                             const tb_font_layout_options *options, tb_font_setup *setup)
{
    tb_font_glyph_metrics space = {0};

    *setup = (tb_font_setup){.font = font, .text = text};
    if (options->pixel_size > 0.0f)
    {
        float multiple = floorf(pixels_per_em / options->pixel_size + 0.5f);
        if (!(multiple >= 1.0f))
        {
            multiple = 1.0f; /* small sizes, and sizes that are not numbers */
        }
        pixels_per_em = multiple * options->pixel_size;
        setup->whole_pixels = true;
    }
    setup->scale = tb_font_scale(font, pixels_per_em);
    setup->pixels_per_em = setup->scale > 0.0f ? pixels_per_em : 0.0f;
    setup->wrap_width = options->wrap_width > 0.0f ? options->wrap_width : 0.0f;
    setup->space = tb_font_find_glyph(font, ' ');
    tb_font_get_glyph_metrics(font, setup->space, &space);
    setup->tab_advance =
        tb_font_pixels(setup, (float)space.advance_width * setup->scale * TB_FONT_TAB_SPACES);
}

static void tb_font_read(const tb_font_setup *setup, size_t offset, tb_font_character *character)
{
    uint32_t code_point = 0;
    size_t end = 0;
    tb_font_glyph_metrics metrics = {0};

    if (!tb_utf8_decode(setup->text.bytes, setup->text.length, offset, &code_point, &end))
    {
        code_point = TB_FONT_REPLACEMENT_CHARACTER;
        end = offset + 1;
    }
    *character = (tb_font_character){.code_point = code_point, .offset = offset, .end = end};
    character->newline = code_point == '\n';
    character->ignored = code_point == '\r';
    character->tab = code_point == '\t';
    character->space = code_point == ' ' || character->tab;
    if (character->newline || character->ignored)
    {
        return;
    }
    if (character->tab)
    {
        character->glyph = setup->space;
        character->advance = setup->tab_advance;
        return;
    }
    character->glyph = tb_font_find_glyph(setup->font, code_point);
    tb_font_get_glyph_metrics(setup->font, character->glyph, &metrics);
    character->advance = tb_font_pixels(setup, (float)metrics.advance_width * setup->scale);
}

/* Where the pen draws a character after the glyph before it: kerned. */
static float tb_font_kerned(const tb_font_setup *setup, float pen, uint32_t previous,
                            const tb_font_character *character)
{
    if (previous == TB_FONT_NO_GLYPH || character->tab)
    {
        return pen;
    }
    return pen + tb_font_pixels(setup, (float)tb_font_get_kerning(setup->font, (uint16_t)previous,
                                                                  character->glyph) *
                                           setup->scale);
}

/*
 * Scan one line from `begin`: place its characters until a \n, the end of
 * the text, or a character that would end past the wrap width. Placed
 * characters are written to `glyphs` when it is given; some past the line's
 * end may be written too, and `glyph_count` says how many are the line's.
 */
static void tb_font_scan_line(const tb_font_setup *setup, size_t begin, size_t line_index,
                              tb_font_layout_glyph *glyphs, tb_font_line_scan *line)
{
    size_t offset = begin;
    size_t written = 0;
    uint32_t previous = TB_FONT_NO_GLYPH;
    float pen = 0.0f;
    float content = 0.0f;      /* the pen after the last character that is not a space */
    bool has_content = false;  /* a character that is not a space is on the line */
    bool after_spaces = false; /* spaces have followed it */
    bool can_break = false;    /* a word has begun after spaces */
    size_t break_at = 0;       /* where that word begins */
    float break_width = 0.0f;  /* the line's width if it breaks there */
    size_t break_glyphs = 0;   /* the line's glyphs if it breaks there */
    tb_font_character character = {0};

    *line = (tb_font_line_scan){.last = true};
    while (offset < setup->text.length)
    {
        float x = 0.0f;
        tb_font_read(setup, offset, &character);
        if (character.newline)
        {
            *line = (tb_font_line_scan){offset, character.end, content, written, false};
            return;
        }
        if (character.ignored)
        {
            offset = character.end;
            continue;
        }
        if (!character.space && after_spaces)
        {
            can_break = true;
            break_at = offset;
            break_width = content;
            break_glyphs = written;
            after_spaces = false;
        }
        x = tb_font_kerned(setup, pen, previous, &character);
        if (!character.space && has_content && setup->wrap_width > 0.0f &&
            x + character.advance > setup->wrap_width + TB_FONT_WRAP_SLACK)
        {
            if (can_break)
            {
                *line = (tb_font_line_scan){break_at, break_at, break_width, break_glyphs, false};
                return;
            }
            *line = (tb_font_line_scan){offset, offset, content, written, false};
            return;
        }
        if (glyphs != NULL)
        {
            glyphs[written] = (tb_font_layout_glyph){.glyph = character.glyph,
                                                     .code_point = character.code_point,
                                                     .offset = character.offset,
                                                     .end = character.end,
                                                     .line = line_index,
                                                     .x = x,
                                                     .advance = character.advance};
        }
        written++;
        pen = x + character.advance;
        if (character.space)
        {
            after_spaces = has_content;
        }
        else
        {
            content = pen;
            has_content = true;
        }
        previous = character.tab ? TB_FONT_NO_GLYPH : character.glyph;
        offset = character.end;
    }
    *line = (tb_font_line_scan){setup->text.length, setup->text.length, content, written, true};
}

static const tb_font_layout_options *tb_font_options_or_default(
    const tb_font_layout_options *options)
{
    static const tb_font_layout_options defaults = {0};
    return options != NULL ? options : &defaults;
}

static float tb_font_line_height(const tb_font_setup *setup, const tb_font_layout_options *options,
                                 float *ascent)
{
    tb_font_line_metrics metrics = {0};
    const float spacing = options->line_spacing > 0.0f ? options->line_spacing : 1.0f;

    tb_font_get_line_metrics(setup->font, setup->pixels_per_em, &metrics);
    *ascent = tb_font_pixels(setup, metrics.ascent);
    return tb_font_pixels(setup, metrics.line_height * spacing);
}

void tb_font_measure_text(const tb_font *font, tb_text text, float pixels_per_em,
                          const tb_font_layout_options *options, tb_font_text_size *size)
{
    tb_font_setup setup = {0};
    tb_font_line_scan line = {0};
    size_t offset = 0;
    float ascent = 0.0f;
    float line_height = 0.0f;

    TB_ASSERT(font != NULL && size != NULL);
    TB_ASSERT(text.bytes != NULL || text.length == 0);
    options = tb_font_options_or_default(options);
    tb_font_setup_of(font, text, pixels_per_em, options, &setup);
    line_height = tb_font_line_height(&setup, options, &ascent);
    *size = (tb_font_text_size){0};
    do
    {
        tb_font_scan_line(&setup, offset, size->line_count, NULL, &line);
        size->width = fmaxf(size->width, line.width);
        size->line_count++;
        offset = line.next;
    } while (!line.last);
    size->height = (float)size->line_count * line_height;
}

void tb_font_layout_text(const tb_font *font, tb_text text, float pixels_per_em,
                         const tb_font_layout_options *options, tb_arena *arena,
                         tb_font_layout *layout)
{
    static const float align_factors[] = {
        [TB_FONT_ALIGN_LEFT] = 0.0f, [TB_FONT_ALIGN_CENTER] = 0.5f, [TB_FONT_ALIGN_RIGHT] = 1.0f};
    tb_font_setup setup = {0};
    tb_font_line_scan scan = {0};
    size_t offset = 0;
    float box = 0.0f;
    float factor = 0.0f;

    TB_ASSERT(font != NULL && arena != NULL && layout != NULL);
    TB_ASSERT(text.bytes != NULL || text.length == 0);
    options = tb_font_options_or_default(options);
    TB_ASSERT_MSG((size_t)options->align < sizeof(align_factors) / sizeof(align_factors[0]),
                  "an alignment of tb_font_align");
    tb_font_setup_of(font, text, pixels_per_em, options, &setup);
    *layout = (tb_font_layout){0};
    layout->pixels_per_em = setup.pixels_per_em;
    layout->mode = setup.whole_pixels ? TB_FONT_RASTER_ALIASED : TB_FONT_RASTER_SMOOTH;
    layout->line_height = tb_font_line_height(&setup, options, &layout->ascent);
    /* No more glyphs than bytes, and no more lines than bytes and one. */
    layout->glyphs = tb_arena_calloc(arena, text.length > 0 ? text.length : 1,
                                     sizeof(*layout->glyphs), _Alignof(tb_font_layout_glyph));
    layout->lines = tb_arena_calloc(arena, text.length + 1, sizeof(*layout->lines),
                                    _Alignof(tb_font_layout_line));
    do
    {
        tb_font_layout_line *const line = &layout->lines[layout->line_count];
        tb_font_scan_line(&setup, offset, layout->line_count, &layout->glyphs[layout->glyph_count],
                          &scan);
        line->first_glyph = layout->glyph_count;
        line->glyph_count = scan.glyph_count;
        line->begin = offset;
        line->end = scan.end;
        line->width = scan.width;
        line->top = (float)layout->line_count * layout->line_height;
        line->baseline = line->top + layout->ascent;
        layout->width = fmaxf(layout->width, scan.width);
        layout->glyph_count += scan.glyph_count;
        layout->line_count++;
        offset = scan.next;
    } while (!scan.last);
    layout->height = (float)layout->line_count * layout->line_height;
    box = setup.wrap_width > 0.0f ? setup.wrap_width : layout->width;
    factor = align_factors[options->align];
    for (size_t index = 0; index < layout->line_count; index++)
    {
        tb_font_layout_line *const line = &layout->lines[index];
        line->x = tb_font_pixels(&setup, (box - line->width) * factor);
    }
}

/* ---------------------------------------------------------------------------
 * Carets.
 * ------------------------------------------------------------------------- */

/* The x of the end of a line, its ending spaces included, from its left. */
static float tb_font_line_end(const tb_font_layout *layout, const tb_font_layout_line *line)
{
    const tb_font_layout_glyph *last = NULL;

    if (line->glyph_count == 0)
    {
        return 0.0f;
    }
    last = &layout->glyphs[line->first_glyph + line->glyph_count - 1];
    return last->x + last->advance;
}

static void tb_font_caret_on(const tb_font_layout *layout, size_t line_index, size_t offset,
                             float x, tb_font_caret *caret)
{
    const tb_font_layout_line *const line = &layout->lines[line_index];

    caret->offset = offset;
    caret->line = line_index;
    caret->x = line->x + x;
    caret->top = line->top;
    caret->bottom = line->top + layout->line_height;
}

void tb_font_layout_caret(const tb_font_layout *layout, size_t offset, tb_font_caret *caret)
{
    size_t line_index = 0;
    const tb_font_layout_line *line = NULL;

    TB_ASSERT(layout != NULL && caret != NULL && layout->line_count > 0);
    /* The last line that begins at or before the offset. */
    while (line_index + 1 < layout->line_count && layout->lines[line_index + 1].begin <= offset)
    {
        line_index++;
    }
    line = &layout->lines[line_index];
    for (size_t index = 0; index < line->glyph_count; index++)
    {
        const tb_font_layout_glyph *const glyph = &layout->glyphs[line->first_glyph + index];
        if (offset < glyph->end)
        {
            tb_font_caret_on(layout, line_index, glyph->offset, glyph->x, caret);
            return;
        }
    }
    tb_font_caret_on(layout, line_index, line->end, tb_font_line_end(layout, line), caret);
}

void tb_font_layout_hit(const tb_font_layout *layout, float x, float y, tb_font_caret *caret)
{
    size_t line_index = 0;
    const tb_font_layout_line *line = NULL;
    float along = 0.0f;

    TB_ASSERT(layout != NULL && caret != NULL && layout->line_count > 0);
    if (layout->line_height > 0.0f && y > 0.0f)
    {
        const float row = floorf(y / layout->line_height);
        line_index = row < (float)(layout->line_count - 1) ? (size_t)row : layout->line_count - 1;
    }
    line = &layout->lines[line_index];
    along = x - line->x;
    for (size_t index = 0; index < line->glyph_count; index++)
    {
        const tb_font_layout_glyph *const glyph = &layout->glyphs[line->first_glyph + index];
        if (along < glyph->x + glyph->advance * 0.5f)
        {
            tb_font_caret_on(layout, line_index, glyph->offset, glyph->x, caret);
            return;
        }
    }
    tb_font_caret_on(layout, line_index, line->end, tb_font_line_end(layout, line), caret);
}
