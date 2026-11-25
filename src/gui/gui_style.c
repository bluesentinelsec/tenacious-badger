/* Styles: themes of style classes, and each widget's style resolved from
 * them for a state, after ttk's styles and Godot's Theme. A widget's
 * styles are resolved once for each set of states asked for and kept until
 * the GUI's style epoch moves on: a theme changed, set, or watched in, or
 * the widget's own class or overrides changed. */
#include "gui_style.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <string.h>

/* Themes, classes, entries, images, and uses held at first. */
#define TB_STYLE_FIRST_CAPACITY 8u
/* The most classes a widget's chain is: its own and those it falls back
 * to. A longer name loses its most specific classes. */
#define TB_STYLE_MOST_CLASSES 16u
/* Sets of states a style is resolved for. */
#define TB_STYLE_STATE_SETS (1u << TB_STYLE_STATE_BITS)
#define TB_STYLE_STATE_MASK (TB_STYLE_STATE_SETS - 1u)

static size_t tb_style_grown(size_t capacity)
{
    return capacity == 0 ? TB_STYLE_FIRST_CAPACITY : capacity * 2u;
}

/* ---------------------------------------------------------------------------
 * Standard values.
 * ------------------------------------------------------------------------- */

static const char *const tb_style_standard_names[TB_STYLE_STANDARD_COUNT] = {
    [TB_STYLE_STANDARD_BACKGROUND] = "background",
    [TB_STYLE_STANDARD_BACKGROUND_IMAGE] = "background_image",
    [TB_STYLE_STANDARD_BORDER_COLOR] = "border_color",
    [TB_STYLE_STANDARD_BORDER_WIDTH] = "border_width",
    [TB_STYLE_STANDARD_RADIUS] = "radius",
    [TB_STYLE_STANDARD_PADDING] = "padding",
    [TB_STYLE_STANDARD_FONT] = "font",
    [TB_STYLE_STANDARD_TEXT_COLOR] = "text_color",
    [TB_STYLE_STANDARD_FOCUS_COLOR] = "focus_color",
    [TB_STYLE_STANDARD_FOCUS_WIDTH] = "focus_width"};

static const tb_style_type tb_style_standard_types[TB_STYLE_STANDARD_COUNT] = {
    [TB_STYLE_STANDARD_BACKGROUND] = TB_STYLE_COLOR,
    [TB_STYLE_STANDARD_BACKGROUND_IMAGE] = TB_STYLE_IMAGE,
    [TB_STYLE_STANDARD_BORDER_COLOR] = TB_STYLE_COLOR,
    [TB_STYLE_STANDARD_BORDER_WIDTH] = TB_STYLE_NUMBER,
    [TB_STYLE_STANDARD_RADIUS] = TB_STYLE_NUMBER,
    [TB_STYLE_STANDARD_PADDING] = TB_STYLE_EDGES,
    [TB_STYLE_STANDARD_FONT] = TB_STYLE_FONT,
    [TB_STYLE_STANDARD_TEXT_COLOR] = TB_STYLE_COLOR,
    [TB_STYLE_STANDARD_FOCUS_COLOR] = TB_STYLE_COLOR,
    [TB_STYLE_STANDARD_FOCUS_WIDTH] = TB_STYLE_NUMBER};

const char *tb_style_standard_name(tb_style_standard standard)
{
    TB_ASSERT(standard < TB_STYLE_STANDARD_COUNT);
    return tb_style_standard_names[standard];
}

tb_style_type tb_style_standard_type(tb_style_standard standard)
{
    TB_ASSERT(standard < TB_STYLE_STANDARD_COUNT);
    return tb_style_standard_types[standard];
}

tb_style_standard tb_style_standard_of(const char *name)
{
    for (size_t index = 0; index < TB_STYLE_STANDARD_COUNT; index++)
    {
        if (tb_str_equal(tb_style_standard_names[index], name))
        {
            return (tb_style_standard)index;
        }
    }
    return TB_STYLE_STANDARD_COUNT;
}

static bool tb_style_type_known(tb_style_type type)
{
    switch (type)
    {
    case TB_STYLE_COLOR:
    case TB_STYLE_NUMBER:
    case TB_STYLE_EDGES:
    case TB_STYLE_FONT:
    case TB_STYLE_IMAGE:
        return true;
    case TB_STYLE_NONE:
        break;
    }
    return false;
}

bool tb_style_value_check(const char *name, const tb_style_value *value, tb_style_value *checked)
{
    const tb_style_standard standard = tb_style_standard_of(name);

    if (!tb_style_type_known(value->type))
    {
        return false;
    }
    if (standard != TB_STYLE_STANDARD_COUNT && tb_style_standard_types[standard] != value->type)
    {
        return false;
    }
    *checked = *value;
    if (checked->type == TB_STYLE_FONT && checked->font.weight != 0 && !(checked->font.size > 0.0f))
    {
        checked->font.size = tb_font_default_role(checked->font.role).size;
    }
    if (checked->type == TB_STYLE_IMAGE)
    {
        const tb_vec4 tint = checked->image.tint;
        if (tint.x == 0.0f && tint.y == 0.0f && tint.z == 0.0f && tint.w == 0.0f)
        {
            checked->image.tint = (tb_vec4){1.0f, 1.0f, 1.0f, 1.0f};
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Themes.
 * ------------------------------------------------------------------------- */

tb_theme *tb_theme_create(void)
{
    tb_theme *const theme = TB_NEW(tb_theme);

    theme->generation = 1;
    return theme;
}

static void tb_theme_class_free(tb_theme_class *style_class)
{
    for (size_t index = 0; index < style_class->count; index++)
    {
        tb_str_free(NULL, style_class->entries[index].name);
    }
    tb_free(style_class->entries);
    tb_str_free(NULL, style_class->name);
    *style_class = (tb_theme_class){0};
}

static void tb_theme_free_classes(tb_theme *theme)
{
    for (size_t index = 0; index < theme->class_count; index++)
    {
        tb_theme_class_free(&theme->classes[index]);
    }
    tb_free(theme->classes);
    theme->classes = NULL;
    theme->class_count = 0;
    theme->class_capacity = 0;
}

void tb_theme_destroy(tb_theme *theme)
{
    if (theme == NULL)
    {
        return;
    }
    tb_theme_free_classes(theme);
    for (size_t index = 0; index < theme->image_count; index++)
    {
        tb_str_free(NULL, theme->images[index].path);
    }
    tb_free(theme->images);
    for (size_t index = 0; index < theme->owned_count; index++)
    {
        tb_image_destroy(theme->renderer, theme->owned[index]);
    }
    tb_free(theme->owned);
    tb_free(theme);
}

void tb_theme_clear(tb_theme *theme)
{
    TB_ASSERT(theme != NULL);
    tb_theme_free_classes(theme);
    theme->generation++;
}

void tb_theme_take_classes(tb_theme *into, tb_theme *from)
{
    tb_theme_free_classes(into);
    into->classes = from->classes;
    into->class_count = from->class_count;
    into->class_capacity = from->class_capacity;
    from->classes = NULL;
    from->class_count = 0;
    from->class_capacity = 0;
    into->generation++;
}

const tb_theme_class *tb_theme_find_class(const tb_theme *theme, const char *name)
{
    for (size_t index = 0; index < theme->class_count; index++)
    {
        if (tb_str_equal(theme->classes[index].name, name))
        {
            return &theme->classes[index];
        }
    }
    return NULL;
}

/* A class found by a name of a length, not terminated. */
static const tb_theme_class *tb_theme_find_span(const tb_theme *theme, const char *name,
                                                size_t length)
{
    for (size_t index = 0; index < theme->class_count; index++)
    {
        const char *const found = theme->classes[index].name;
        if (tb_str_length(found) == length && memcmp(found, name, length) == 0)
        {
            return &theme->classes[index];
        }
    }
    return NULL;
}

static tb_theme_class *tb_theme_class_for(tb_theme *theme, const char *name)
{
    const tb_theme_class *const found = tb_theme_find_class(theme, name);

    if (found != NULL)
    {
        return &theme->classes[found - theme->classes];
    }
    if (theme->class_count == theme->class_capacity)
    {
        theme->class_capacity = tb_style_grown(theme->class_capacity);
        theme->classes =
            tb_realloc_array(theme->classes, theme->class_capacity, sizeof(*theme->classes));
    }
    tb_theme_class *const made = &theme->classes[theme->class_count];
    *made = (tb_theme_class){.name = tb_str_dup(NULL, name)};
    theme->class_count++;
    return made;
}

/* Set an entry of a list, replacing the one of the same name and states. */
static void tb_style_entries_set(tb_gui_style_entry **entries, size_t *count, size_t *capacity,
                                 const char *name, uint32_t states, const tb_style_value *value)
{
    for (size_t index = 0; index < *count; index++)
    {
        tb_gui_style_entry *const entry = &(*entries)[index];
        if (entry->states == states && tb_str_equal(entry->name, name))
        {
            entry->value = *value;
            return;
        }
    }
    if (*count == *capacity)
    {
        *capacity = tb_style_grown(*capacity);
        *entries = tb_realloc_array(*entries, *capacity, sizeof(**entries));
    }
    (*entries)[*count] =
        (tb_gui_style_entry){.name = tb_str_dup(NULL, name), .states = states, .value = *value};
    (*count)++;
}

static bool tb_style_states_valid(uint32_t states)
{
    return (states & ~TB_STYLE_STATE_MASK) == 0;
}

bool tb_theme_set(tb_theme *theme, const char *style_class, const char *name, uint32_t states,
                  const tb_style_value *value)
{
    tb_style_value checked = {0};

    TB_ASSERT(theme != NULL);
    TB_ASSERT(style_class != NULL);
    TB_ASSERT(name != NULL);
    TB_ASSERT(value != NULL);
    if (!tb_style_states_valid(states) || !tb_style_value_check(name, value, &checked))
    {
        return false;
    }
    tb_theme_class *const found = tb_theme_class_for(theme, style_class);
    tb_style_entries_set(&found->entries, &found->count, &found->capacity, name, states, &checked);
    theme->generation++;
    return true;
}

bool tb_theme_set_color(tb_theme *theme, const char *style_class, const char *name, uint32_t states,
                        tb_vec4 color)
{
    const tb_style_value value = {.type = TB_STYLE_COLOR, .color = color};
    return tb_theme_set(theme, style_class, name, states, &value);
}

bool tb_theme_set_number(tb_theme *theme, const char *style_class, const char *name,
                         uint32_t states, float number)
{
    const tb_style_value value = {.type = TB_STYLE_NUMBER, .number = number};
    return tb_theme_set(theme, style_class, name, states, &value);
}

bool tb_theme_set_edges(tb_theme *theme, const char *style_class, const char *name, uint32_t states,
                        tb_gui_edges edges)
{
    const tb_style_value value = {.type = TB_STYLE_EDGES, .edges = edges};
    return tb_theme_set(theme, style_class, name, states, &value);
}

bool tb_theme_set_font(tb_theme *theme, const char *style_class, const char *name, uint32_t states,
                       tb_font_spec font)
{
    const tb_style_value value = {.type = TB_STYLE_FONT, .font = font};
    return tb_theme_set(theme, style_class, name, states, &value);
}

bool tb_theme_set_image(tb_theme *theme, const char *style_class, const char *name, uint32_t states,
                        const tb_style_image *image)
{
    TB_ASSERT(image != NULL);
    const tb_style_value value = {.type = TB_STYLE_IMAGE, .image = *image};
    return tb_theme_set(theme, style_class, name, states, &value);
}

bool tb_theme_get(const tb_theme *theme, const char *style_class, const char *name, uint32_t states,
                  tb_style_value *value)
{
    TB_ASSERT(theme != NULL);
    TB_ASSERT(style_class != NULL);
    TB_ASSERT(name != NULL);
    TB_ASSERT(value != NULL);
    *value = (tb_style_value){0};
    const tb_theme_class *const found = tb_theme_find_class(theme, style_class);
    if (found == NULL)
    {
        return false;
    }
    for (size_t index = 0; index < found->count; index++)
    {
        if (found->entries[index].states == states &&
            tb_str_equal(found->entries[index].name, name))
        {
            *value = found->entries[index].value;
            return true;
        }
    }
    return false;
}

void tb_theme_add_image(tb_theme *theme, const char *path, const tb_image *image)
{
    TB_ASSERT(theme != NULL);
    TB_ASSERT(path != NULL);
    TB_ASSERT(image != NULL);
    for (size_t index = 0; index < theme->image_count; index++)
    {
        if (tb_str_equal(theme->images[index].path, path))
        {
            theme->images[index].image = image;
            theme->generation++;
            return;
        }
    }
    if (theme->image_count == theme->image_capacity)
    {
        theme->image_capacity = tb_style_grown(theme->image_capacity);
        theme->images =
            tb_realloc_array(theme->images, theme->image_capacity, sizeof(*theme->images));
    }
    theme->images[theme->image_count] =
        (tb_theme_image_name){.path = tb_str_dup(NULL, path), .image = image};
    theme->image_count++;
    theme->generation++;
}

const tb_image *tb_theme_image(const tb_theme *theme, const char *path)
{
    TB_ASSERT(theme != NULL);
    TB_ASSERT(path != NULL);
    for (size_t index = 0; index < theme->image_count; index++)
    {
        if (tb_str_equal(theme->images[index].path, path))
        {
            return theme->images[index].image;
        }
    }
    return NULL;
}

const tb_image *tb_theme_make_image(tb_theme *theme, tb_renderer *renderer,
                                    const tb_image_desc *desc, const uint8_t *pixels, size_t size)
{
    tb_image *const image = tb_image_create(renderer, desc, pixels, size);

    TB_ASSERT(theme->renderer == NULL || theme->renderer == renderer);
    theme->renderer = renderer;
    if (theme->owned_count == theme->owned_capacity)
    {
        theme->owned_capacity = tb_style_grown(theme->owned_capacity);
        theme->owned = tb_realloc_array(theme->owned, theme->owned_capacity, sizeof(*theme->owned));
    }
    theme->owned[theme->owned_count] = image;
    theme->owned_count++;
    return image;
}

uint64_t tb_theme_generation(const tb_theme *theme)
{
    TB_ASSERT(theme != NULL);
    return theme->generation;
}

/* ---------------------------------------------------------------------------
 * The themes a GUI uses.
 * ------------------------------------------------------------------------- */

static void tb_gui_styles_changed(tb_gui *gui)
{
    gui->style_epoch++;
    tb_gui_mark_layout(gui);
}

static void tb_gui_theme_acquire(tb_gui *gui, const tb_theme *theme)
{
    for (size_t index = 0; index < gui->theme_use_count; index++)
    {
        if (gui->theme_uses[index].theme == theme)
        {
            gui->theme_uses[index].count++;
            return;
        }
    }
    if (gui->theme_use_count == gui->theme_use_capacity)
    {
        gui->theme_use_capacity = tb_style_grown(gui->theme_use_capacity);
        gui->theme_uses =
            tb_realloc_array(gui->theme_uses, gui->theme_use_capacity, sizeof(*gui->theme_uses));
    }
    gui->theme_uses[gui->theme_use_count] =
        (tb_gui_theme_use){.theme = theme, .count = 1, .generation = theme->generation};
    gui->theme_use_count++;
}

static void tb_gui_theme_release(tb_gui *gui, const tb_theme *theme)
{
    for (size_t index = 0; index < gui->theme_use_count; index++)
    {
        tb_gui_theme_use *const use = &gui->theme_uses[index];
        if (use->theme != theme)
        {
            continue;
        }
        use->count--;
        if (use->count == 0)
        {
            gui->theme_uses[index] = gui->theme_uses[gui->theme_use_count - 1];
            gui->theme_use_count--;
        }
        return;
    }
}

void tb_gui_styles_init(tb_gui *gui, tb_theme *theme)
{
    gui->style_epoch = 1;
    gui->default_theme = tb_theme_create_tool(&gui->theme);
    tb_gui_theme_acquire(gui, gui->default_theme);
    if (theme != NULL)
    {
        tb_gui_set_style_theme(gui, theme);
    }
}

void tb_gui_styles_free(tb_gui *gui)
{
    tb_theme_destroy(gui->default_theme);
    gui->default_theme = NULL;
    tb_free(gui->theme_uses);
    gui->theme_uses = NULL;
    gui->theme_use_count = 0;
    tb_free(gui->theme_chain);
    gui->theme_chain = NULL;
}

void tb_gui_styles_palette_changed(tb_gui *gui)
{
    tb_theme *const made = tb_theme_create_tool(&gui->theme);

    tb_gui_theme_release(gui, gui->default_theme);
    tb_theme_destroy(gui->default_theme);
    gui->default_theme = made;
    tb_gui_theme_acquire(gui, made);
    tb_gui_styles_changed(gui);
}

void tb_gui_styles_refresh(tb_gui *gui)
{
    bool changed = false;

    for (size_t index = 0; index < gui->theme_use_count; index++)
    {
        tb_gui_theme_use *const use = &gui->theme_uses[index];
        if (use->generation != use->theme->generation)
        {
            use->generation = use->theme->generation;
            changed = true;
        }
    }
    if (changed)
    {
        tb_gui_styles_changed(gui);
    }
}

void tb_gui_set_style_theme(tb_gui *gui, tb_theme *theme)
{
    TB_ASSERT(gui != NULL);
    if (gui->style_theme == theme)
    {
        return;
    }
    if (gui->style_theme != NULL)
    {
        tb_gui_theme_release(gui, gui->style_theme);
    }
    gui->style_theme = theme;
    if (theme != NULL)
    {
        tb_gui_theme_acquire(gui, theme);
    }
    tb_gui_styles_changed(gui);
}

tb_theme *tb_gui_style_theme(const tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    return gui->style_theme;
}

const tb_theme *tb_gui_default_style_theme(const tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    return gui->default_theme;
}

bool tb_widget_set_style_theme(tb_widget widget, tb_theme *theme)
{
    tb_gui_node *const node = tb_gui_node_of(widget);

    if (node == NULL)
    {
        return false;
    }
    if (node->style_theme == theme)
    {
        return true;
    }
    if (node->style_theme != NULL)
    {
        tb_gui_theme_release(widget.gui, node->style_theme);
    }
    node->style_theme = theme;
    if (theme != NULL)
    {
        tb_gui_theme_acquire(widget.gui, theme);
    }
    tb_gui_styles_changed(widget.gui);
    return true;
}

tb_theme *tb_widget_style_theme(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL ? node->style_theme : NULL;
}

/* ---------------------------------------------------------------------------
 * A widget's own.
 * ------------------------------------------------------------------------- */

struct tb_gui_style_cache
{
    uint64_t epoch;
    tb_style *styles[TB_STYLE_STATE_SETS];
};

/* A style and its elements are one allocation: the elements after the
 * style, at their alignment. */
static size_t tb_style_elements_offset(void)
{
    const size_t align = _Alignof(tb_style_value);
    return (sizeof(tb_style) + align - 1u) / align * align;
}

static void tb_gui_style_cache_clear(tb_gui_style_cache *cache)
{
    for (size_t index = 0; index < TB_STYLE_STATE_SETS; index++)
    {
        tb_free(cache->styles[index]);
        cache->styles[index] = NULL;
    }
    cache->epoch = 0;
}

void tb_gui_style_forget(tb_gui *gui, uint32_t index)
{
    tb_gui_node *const node = tb_gui_node_at(gui, index);

    if (node->style_cache != NULL)
    {
        tb_gui_style_cache_clear(node->style_cache);
    }
    tb_gui_mark_layout(gui);
}

void tb_gui_style_release(tb_gui *gui, uint32_t index)
{
    tb_gui_node *const node = tb_gui_node_at(gui, index);

    if (node->style_cache != NULL)
    {
        tb_gui_style_cache_clear(node->style_cache);
        tb_free(node->style_cache);
        node->style_cache = NULL;
    }
    for (size_t entry = 0; entry < node->override_count; entry++)
    {
        tb_str_free(NULL, node->overrides[entry].name);
    }
    tb_free(node->overrides);
    node->overrides = NULL;
    node->override_count = 0;
    node->override_capacity = 0;
    if (node->style_theme != NULL)
    {
        tb_gui_theme_release(gui, node->style_theme);
        node->style_theme = NULL;
    }
}

bool tb_widget_override_style(tb_widget widget, const char *name, uint32_t states,
                              const tb_style_value *value)
{
    tb_gui_node *const node = tb_gui_node_of(widget);
    tb_style_value checked = {0};

    TB_ASSERT(name != NULL);
    if (node == NULL || !tb_style_states_valid(states))
    {
        return false;
    }
    if (value == NULL)
    {
        for (size_t index = 0; index < node->override_count; index++)
        {
            tb_gui_style_entry *const entry = &node->overrides[index];
            if (entry->states == states && tb_str_equal(entry->name, name))
            {
                tb_str_free(NULL, entry->name);
                node->overrides[index] = node->overrides[node->override_count - 1];
                node->override_count--;
                break;
            }
        }
        tb_gui_style_forget(widget.gui, widget.index);
        return true;
    }
    if (!tb_style_value_check(name, value, &checked))
    {
        return false;
    }
    tb_style_entries_set(&node->overrides, &node->override_count, &node->override_capacity, name,
                         states, &checked);
    tb_gui_style_forget(widget.gui, widget.index);
    return true;
}

void tb_widget_clear_style_overrides(tb_widget widget)
{
    tb_gui_node *const node = tb_gui_node_of(widget);

    if (node == NULL)
    {
        return;
    }
    for (size_t index = 0; index < node->override_count; index++)
    {
        tb_str_free(NULL, node->overrides[index].name);
    }
    node->override_count = 0;
    tb_gui_style_forget(widget.gui, widget.index);
}

/* ---------------------------------------------------------------------------
 * Resolving.
 * ------------------------------------------------------------------------- */

/* A widget's class chain: spans of its style's name, most specific first,
 * then its kind's class, then the root. */
typedef struct tb_style_chain
{
    const char *names[TB_STYLE_MOST_CLASSES + 2u];
    size_t lengths[TB_STYLE_MOST_CLASSES + 2u];
    size_t count;
} tb_style_chain;

static void tb_style_chain_add(tb_style_chain *chain, const char *name, size_t length)
{
    chain->names[chain->count] = name;
    chain->lengths[chain->count] = length;
    chain->count++;
}

static tb_style_chain tb_style_chain_of(const tb_gui_node *node)
{
    tb_style_chain chain = {0};
    const tb_widget_style_desc *const desc = node->kind->style;
    const char *const kind_class = desc != NULL ? desc->name : NULL;
    const tb_text own = tb_gui_string_text(&node->style);
    const char *const name = own.length > 0 ? own.bytes : kind_class;
    const size_t length = own.length > 0 ? own.length : (name != NULL ? tb_str_length(name) : 0);
    size_t last = 0;

    /* "Primary.Button", then "Button": each suffix after a dot. */
    for (size_t at = 0; at < length && chain.count < TB_STYLE_MOST_CLASSES; at++)
    {
        if (at == 0 || name[at - 1] == '.')
        {
            last = chain.count;
            tb_style_chain_add(&chain, &name[at], length - at);
        }
    }
    if (kind_class != NULL)
    {
        const size_t kind_length = tb_str_length(kind_class);
        const bool ends_in_kind = chain.count > 0 && chain.lengths[last] == kind_length &&
                                  memcmp(chain.names[last], kind_class, kind_length) == 0;
        if (!ends_in_kind)
        {
            tb_style_chain_add(&chain, kind_class, kind_length);
        }
    }
    tb_style_chain_add(&chain, TB_STYLE_ROOT_CLASS, tb_str_length(TB_STYLE_ROOT_CLASS));
    return chain;
}

/* The themes of a widget, nearest first, into the GUI's chain. */
static size_t tb_style_themes_of(tb_gui *gui, uint32_t index)
{
    size_t count = 0;

    for (uint32_t at = index; at != TB_GUI_NO_NODE; at = tb_gui_node_at(gui, at)->parent)
    {
        const tb_theme *const theme = tb_gui_node_at(gui, at)->style_theme;
        if (theme == NULL)
        {
            continue;
        }
        if (count == gui->theme_chain_capacity)
        {
            gui->theme_chain_capacity = tb_style_grown(gui->theme_chain_capacity);
            gui->theme_chain = tb_realloc_array(gui->theme_chain, gui->theme_chain_capacity,
                                                sizeof(*gui->theme_chain));
        }
        gui->theme_chain[count] = theme;
        count++;
    }
    const tb_theme *const last[] = {gui->style_theme, gui->default_theme};
    for (size_t at = 0; at < sizeof(last) / sizeof(last[0]); at++)
    {
        if (last[at] == NULL)
        {
            continue;
        }
        if (count == gui->theme_chain_capacity)
        {
            gui->theme_chain_capacity = tb_style_grown(gui->theme_chain_capacity);
            gui->theme_chain = tb_realloc_array(gui->theme_chain, gui->theme_chain_capacity,
                                                sizeof(*gui->theme_chain));
        }
        gui->theme_chain[count] = last[at];
        count++;
    }
    return count;
}

/* How an entry's states rank against another's: disabled outranks
 * selected, selected pressed, pressed hovered, and hovered focused, so that
 * a state outranks every mix of those below it, as Godot's styles of a
 * button do. */
static int32_t tb_style_rank(uint32_t states)
{
    static const uint32_t order[TB_STYLE_STATE_BITS] = {TB_WIDGET_FOCUSED, TB_WIDGET_HOVERED,
                                                        TB_WIDGET_PRESSED, TB_WIDGET_SELECTED,
                                                        TB_WIDGET_DISABLED};
    int32_t rank = 0;

    for (size_t index = 0; index < TB_STYLE_STATE_BITS; index++)
    {
        if ((states & order[index]) != 0)
        {
            rank |= 1 << index;
        }
    }
    return rank;
}

/* The best entry of a list for a name in a set of states, against the best
 * found so far: of a higher rank, or of the same and found first. */
static void tb_style_consider(const tb_gui_style_entry *entries, size_t count, const char *name,
                              uint32_t states, const tb_style_value **best, int32_t *best_rank)
{
    for (size_t index = 0; index < count; index++)
    {
        const tb_gui_style_entry *const entry = &entries[index];
        if ((entry->states & ~states) != 0 || !tb_str_equal(entry->name, name))
        {
            continue;
        }
        const int32_t rank = tb_style_rank(entry->states);
        if (rank > *best_rank)
        {
            *best = &entry->value;
            *best_rank = rank;
        }
    }
}

/* Where a widget's values are looked for, in order: its overrides, then
 * each class of each theme. */
typedef struct tb_style_sources
{
    const tb_gui_node *node;
    const tb_theme_class **classes;
    size_t class_count;
} tb_style_sources;

static const tb_style_value *tb_style_pick(const tb_style_sources *sources, const char *name,
                                           uint32_t states)
{
    const tb_style_value *best = NULL;
    int32_t best_rank = -1;
    const int32_t most = tb_style_rank(states);

    tb_style_consider(sources->node->overrides, sources->node->override_count, name, states, &best,
                      &best_rank);
    for (size_t index = 0; index < sources->class_count && best_rank < most; index++)
    {
        const tb_theme_class *const found = sources->classes[index];
        tb_style_consider(found->entries, found->count, name, states, &best, &best_rank);
    }
    return best;
}

static void tb_style_take(tb_style *style, tb_style_standard standard, const tb_style_value *value)
{
    switch (standard)
    {
    case TB_STYLE_STANDARD_BACKGROUND:
        style->background = value->color;
        break;
    case TB_STYLE_STANDARD_BACKGROUND_IMAGE:
        style->image = value->image;
        break;
    case TB_STYLE_STANDARD_BORDER_COLOR:
        style->border_color = value->color;
        break;
    case TB_STYLE_STANDARD_BORDER_WIDTH:
        style->border_width = value->number;
        break;
    case TB_STYLE_STANDARD_RADIUS:
        style->radius = value->number;
        break;
    case TB_STYLE_STANDARD_PADDING:
        style->padding = value->edges;
        break;
    case TB_STYLE_STANDARD_FONT:
        style->font = value->font;
        break;
    case TB_STYLE_STANDARD_TEXT_COLOR:
        style->text_color = value->color;
        break;
    case TB_STYLE_STANDARD_FOCUS_COLOR:
        style->focus_color = value->color;
        break;
    case TB_STYLE_STANDARD_FOCUS_WIDTH:
        style->focus_width = value->number;
        break;
    case TB_STYLE_STANDARD_COUNT:
        break;
    }
}

static tb_style *tb_style_resolve(tb_gui *gui, uint32_t index, uint32_t states)
{
    const tb_gui_node *const node = tb_gui_node_at(gui, index);
    const tb_widget_style_desc *const desc = node->kind->style;
    const size_t element_count = desc != NULL ? desc->element_count : 0;
    const tb_style_chain chain = tb_style_chain_of(node);
    const size_t theme_count = tb_style_themes_of(gui, index);
    const tb_theme_class **const classes = tb_calloc(theme_count, chain.count * sizeof(*classes));
    tb_style_sources sources = {.node = node, .classes = classes};
    char *block = NULL;

    /* A kind declares its elements in a table of its own: a few. */
    TB_ASSERT(element_count < SIZE_MAX / sizeof(tb_style_value) - tb_style_elements_offset());
    block = tb_calloc(1, tb_style_elements_offset() + element_count * sizeof(tb_style_value));
    tb_style *const style = (tb_style *)(void *)block;
    tb_style_value *const elements = (tb_style_value *)(void *)(block + tb_style_elements_offset());

    for (size_t theme = 0; theme < theme_count; theme++)
    {
        for (size_t link = 0; link < chain.count; link++)
        {
            const tb_theme_class *const found =
                tb_theme_find_span(gui->theme_chain[theme], chain.names[link], chain.lengths[link]);
            if (found != NULL)
            {
                classes[sources.class_count] = found;
                sources.class_count++;
            }
        }
    }
    for (size_t standard = 0; standard < TB_STYLE_STANDARD_COUNT; standard++)
    {
        const tb_style_value *const value =
            tb_style_pick(&sources, tb_style_standard_names[standard], states);
        if (value != NULL)
        {
            tb_style_take(style, (tb_style_standard)standard, value);
        }
    }
    for (size_t element = 0; element < element_count; element++)
    {
        const tb_style_value *const value =
            tb_style_pick(&sources, desc->elements[element].name, states);
        if (value != NULL && value->type == desc->elements[element].type)
        {
            elements[element] = *value;
        }
    }
    style->desc = desc;
    style->elements = elements;
    style->element_count = element_count;
    tb_free(classes);
    return style;
}

const tb_style *tb_style_in(tb_widget widget, uint32_t states)
{
    tb_gui_node *const node = tb_gui_node_of(widget);

    if (node == NULL)
    {
        return NULL;
    }
    const tb_widget_style_desc *const desc = node->kind->style;
    const uint32_t drawn = (states & TB_STYLE_STATE_MASK) & (desc != NULL ? desc->states : ~0u);
    if (node->style_cache == NULL)
    {
        node->style_cache = TB_NEW(tb_gui_style_cache);
    }
    tb_gui_style_cache *const cache = node->style_cache;
    if (cache->epoch != widget.gui->style_epoch)
    {
        tb_gui_style_cache_clear(cache);
        cache->epoch = widget.gui->style_epoch;
    }
    TB_ASSERT(drawn < TB_STYLE_STATE_SETS);
    if (cache->styles[drawn] == NULL)
    {
        cache->styles[drawn] = tb_style_resolve(widget.gui, widget.index, drawn);
    }
    return cache->styles[drawn];
}

const tb_style *tb_style_of(tb_widget widget)
{
    return tb_style_in(widget, tb_widget_state(widget));
}

const tb_image *tb_gui_style_image_named(tb_widget widget, const char *path)
{
    if (!tb_widget_alive(widget))
    {
        return NULL;
    }
    const size_t count = tb_style_themes_of(widget.gui, widget.index);
    for (size_t index = 0; index < count; index++)
    {
        const tb_image *const image = tb_theme_image(widget.gui->theme_chain[index], path);
        if (image != NULL)
        {
            return image;
        }
    }
    return NULL;
}

const char *tb_gui_style_image_path(tb_widget widget, const tb_image *image)
{
    if (!tb_widget_alive(widget))
    {
        return NULL;
    }
    const size_t count = tb_style_themes_of(widget.gui, widget.index);
    for (size_t index = 0; index < count; index++)
    {
        const tb_theme *const theme = widget.gui->theme_chain[index];
        for (size_t at = 0; at < theme->image_count; at++)
        {
            if (theme->images[at].image == image)
            {
                return theme->images[at].path;
            }
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Elements.
 * ------------------------------------------------------------------------- */

static const tb_style_value *tb_style_element_typed(const tb_style *style, size_t element,
                                                    tb_style_type type)
{
    TB_ASSERT(style != NULL);
    if (element >= style->element_count || style->elements[element].type != type)
    {
        return NULL;
    }
    return &style->elements[element];
}

tb_vec4 tb_style_color(const tb_style *style, size_t element)
{
    const tb_style_value *const value = tb_style_element_typed(style, element, TB_STYLE_COLOR);
    return value != NULL ? value->color : (tb_vec4){0};
}

float tb_style_number(const tb_style *style, size_t element)
{
    const tb_style_value *const value = tb_style_element_typed(style, element, TB_STYLE_NUMBER);
    return value != NULL ? value->number : 0.0f;
}

tb_gui_edges tb_style_edges(const tb_style *style, size_t element)
{
    const tb_style_value *const value = tb_style_element_typed(style, element, TB_STYLE_EDGES);
    return value != NULL ? value->edges : (tb_gui_edges){0};
}

tb_font_spec tb_style_font(const tb_style *style, size_t element)
{
    const tb_style_value *const value = tb_style_element_typed(style, element, TB_STYLE_FONT);
    return value != NULL ? value->font : (tb_font_spec){0};
}

const tb_style_image *tb_style_image_of(const tb_style *style, size_t element)
{
    const tb_style_value *const value = tb_style_element_typed(style, element, TB_STYLE_IMAGE);
    return value != NULL ? &value->image : NULL;
}

const tb_style_value *tb_style_find(const tb_style *style, const char *name)
{
    TB_ASSERT(style != NULL);
    TB_ASSERT(name != NULL);
    for (size_t index = 0; index < style->element_count; index++)
    {
        if (tb_str_equal(style->desc->elements[index].name, name))
        {
            return &style->elements[index];
        }
    }
    return NULL;
}

tb_vec4 tb_gui_style_color_named(const tb_style *style, const char *name)
{
    const tb_style_value *const value = tb_style_find(style, name);
    return value != NULL && value->type == TB_STYLE_COLOR ? value->color : (tb_vec4){0};
}

float tb_gui_style_number_named(const tb_style *style, const char *name)
{
    const tb_style_value *const value = tb_style_find(style, name);
    return value != NULL && value->type == TB_STYLE_NUMBER ? value->number : 0.0f;
}

/* ---------------------------------------------------------------------------
 * Drawing, and text.
 * ------------------------------------------------------------------------- */

void tb_gui_paint_nine_slice(tb_gui_paint *paint, const tb_canvas_nine_slice *slice,
                             tb_canvas_rect rect, tb_vec4 tint)
{
    TB_ASSERT(paint != NULL);
    TB_ASSERT(slice != NULL);
    tb_ui_paint_nine_slice(&paint->gui->paint, paint->layer, paint->clip, slice, rect, tint,
                           (tb_ui_id)paint->widget.index + 1u);
}

void tb_gui_paint_box(tb_gui_paint *paint, tb_canvas_rect rect, const tb_style *style)
{
    TB_ASSERT(paint != NULL);
    TB_ASSERT(style != NULL);
    if (style->image.slice.image != NULL)
    {
        tb_gui_paint_nine_slice(paint, &style->image.slice, rect, style->image.tint);
    }
    else
    {
        tb_gui_paint_fill(paint, rect, style->radius, style->background);
    }
    if (style->border_width > 0.0f)
    {
        tb_gui_paint_outline(paint, rect, style->radius, style->border_width, style->border_color);
    }
    if (style->focus_width > 0.0f)
    {
        tb_gui_paint_outline(paint, rect, style->radius, style->focus_width, style->focus_color);
    }
}

tb_gui_resolved_font tb_gui_widget_font(tb_widget widget, tb_font_spec spec)
{
    const tb_style *const style = tb_style_in(widget, 0);

    return tb_gui_resolve_font(widget.gui, style != NULL ? style->font : (tb_font_spec){0}, spec);
}
