/* The image kind: a canvas image drawn in a widget, at its own size or
 * fitted to the widget's, placed by an anchor and tinted, after Tk's
 * `label` of an image and CSS's `object-fit`. */
#include "gui_internal.h"

#include "badger/assert.h"

#include <math.h>
#include <stddef.h>

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

static const char *const tb_gui_fit_names[] = {"none", "contain", "cover", "fill", "scale_down"};
static const char *const tb_gui_image_anchor_names[] = {"center", "n",  "ne", "e", "se",
                                                        "s",      "sw", "w",  "nw"};

typedef struct tb_gui_image
{
    const tb_image *image;
    int32_t fit;
    int32_t anchor;
    tb_vec4 tint;
    float width;
    float height;
} tb_gui_image;

static tb_gui_image *tb_gui_image_of(tb_widget widget);

static void tb_gui_image_init(tb_widget widget, void *data)
{
    tb_gui_image *const image = data;

    (void)widget;
    image->tint = (tb_vec4){1.0f, 1.0f, 1.0f, 1.0f};
}

/* The image's size in units: a pixel a unit. */
static tb_vec2 tb_gui_image_size(const tb_gui_image *image)
{
    uint32_t width = 0;
    uint32_t height = 0;

    if (image->image == NULL)
    {
        return (tb_vec2){0};
    }
    tb_image_size(image->image, &width, &height);
    return (tb_vec2){(float)width, (float)height};
}

static tb_vec2 tb_gui_image_measure(tb_widget widget, const void *data)
{
    const tb_gui_image *const image = data;
    const tb_vec2 size = tb_gui_image_size(image);

    (void)widget;
    return (tb_vec2){image->width > 0.0f ? image->width : size.x,
                     image->height > 0.0f ? image->height : size.y};
}

tb_canvas_rect tb_gui_fit_rect(tb_vec2 size, tb_canvas_rect rect, tb_image_fit fit,
                               tb_gui_anchor anchor)
{
    tb_vec2 drawn = size;

    if (!(size.x > 0.0f) || !(size.y > 0.0f))
    {
        return (tb_canvas_rect){rect.x, rect.y, 0.0f, 0.0f};
    }
    const float contain = fminf(rect.width / size.x, rect.height / size.y);
    const float cover = fmaxf(rect.width / size.x, rect.height / size.y);
    switch (fit)
    {
    case TB_IMAGE_FIT_CONTAIN:
        drawn = (tb_vec2){size.x * contain, size.y * contain};
        break;
    case TB_IMAGE_FIT_COVER:
        drawn = (tb_vec2){size.x * cover, size.y * cover};
        break;
    case TB_IMAGE_FIT_FILL:
        drawn = (tb_vec2){rect.width, rect.height};
        break;
    case TB_IMAGE_FIT_SCALE_DOWN:
        drawn = contain < 1.0f ? (tb_vec2){size.x * contain, size.y * contain} : size;
        break;
    case TB_IMAGE_FIT_NONE:
        break;
    }
    const float across = tb_gui_anchor_fraction((int32_t)anchor, TB_GUI_COLUMNS);
    const float down = tb_gui_anchor_fraction((int32_t)anchor, TB_GUI_ROWS);
    return (tb_canvas_rect){rect.x + (rect.width - drawn.x) * across,
                            rect.y + (rect.height - drawn.y) * down, drawn.x, drawn.y};
}

static void tb_gui_image_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_image *const image = data;
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const bool disabled = (tb_widget_state(widget) & TB_WIDGET_DISABLED) != 0;
    tb_vec4 tint = image->tint;

    if (image->image == NULL)
    {
        return;
    }
    /* A disabled image is dimmed, as text is. */
    tint.w *= disabled ? 0.5f : 1.0f;
    tb_gui_paint_image(paint, image->image,
                       tb_gui_fit_rect(tb_gui_image_size(image), rect, (tb_image_fit)image->fit,
                                       (tb_gui_anchor)image->anchor),
                       tint);
}

static bool tb_gui_image_set_size(float *slot, const tb_gui_value *value)
{
    if (value->number < 0.0f)
    {
        return false;
    }
    *slot = value->number;
    return true;
}

static bool tb_gui_image_set_width(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_image *const image = data;

    (void)widget;
    return tb_gui_image_set_size(&image->width, value);
}

static bool tb_gui_image_get_width(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_image *const image = data;

    (void)widget;
    *value = tb_gui_float(image->width);
    return true;
}

static bool tb_gui_image_set_height(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_image *const image = data;

    (void)widget;
    return tb_gui_image_set_size(&image->height, value);
}

static bool tb_gui_image_get_height(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_image *const image = data;

    (void)widget;
    *value = tb_gui_float(image->height);
    return true;
}

static const tb_gui_property tb_gui_image_properties[] = {
    {.name = "fit",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(tb_gui_image, fit),
     .names = tb_gui_fit_names,
     .name_count = TB_GUI_COUNT_OF(tb_gui_fit_names)},
    {.name = "anchor",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(tb_gui_image, anchor),
     .names = tb_gui_image_anchor_names,
     .name_count = TB_GUI_COUNT_OF(tb_gui_image_anchor_names)},
    {.name = "tint", .type = TB_GUI_TYPE_COLOR, .offset = offsetof(tb_gui_image, tint)},
    {.name = "width",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_image_get_width,
     .set = tb_gui_image_set_width},
    {.name = "height",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_image_get_height,
     .set = tb_gui_image_set_height},
};

static const tb_widget_class tb_gui_image_kind = {.name = "image",
                                                  .data_size = sizeof(tb_gui_image),
                                                  .properties = tb_gui_image_properties,
                                                  .property_count =
                                                      TB_GUI_COUNT_OF(tb_gui_image_properties),
                                                  .mouse_filter = TB_GUI_MOUSE_PASS,
                                                  .init = tb_gui_image_init,
                                                  .measure = tb_gui_image_measure,
                                                  .draw = tb_gui_image_draw};

static tb_gui_image *tb_gui_image_of(tb_widget widget)
{
    return tb_widget_class_of(widget) == &tb_gui_image_kind ? tb_widget_data(widget) : NULL;
}

const tb_widget_class *tb_image_widget_class(void)
{
    return &tb_gui_image_kind;
}

tb_widget tb_image_widget(tb_widget parent, const tb_image_widget_desc *desc)
{
    const tb_image_widget_desc chosen = desc != NULL ? *desc : (tb_image_widget_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_image_kind);
    const tb_gui_value values[] = {tb_gui_enum((int32_t)chosen.fit),
                                   tb_gui_enum((int32_t)chosen.anchor), tb_gui_float(chosen.width),
                                   tb_gui_float(chosen.height)};
    static const char *const names[] = {"fit", "anchor", "width", "height"};

    for (size_t index = 0; index < TB_GUI_COUNT_OF(names); index++)
    {
        (void)tb_widget_set(widget, names[index], &values[index]);
    }
    (void)tb_image_widget_set_image(widget, chosen.image);
    return widget;
}

bool tb_image_widget_set_image(tb_widget widget, const tb_image *image)
{
    tb_gui_image *const data = tb_gui_image_of(widget);

    if (data == NULL)
    {
        return false;
    }
    data->image = image;
    tb_gui_mark_layout(widget.gui);
    return true;
}

const tb_image *tb_image_widget_image(tb_widget widget)
{
    const tb_gui_image *const data = tb_gui_image_of(widget);
    return data != NULL ? data->image : NULL;
}

void tb_gui_register_image(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_image_kind);
}
