/* A catalog of fonts: the embedded fonts and those a game adds, found by
 * family, weight, and slant; and the role table. */
#include "badger/font_catalog.h"
#include "badger/array.h"
#include "badger/assert.h"
#include "badger/font.h"
#include "badger/str.h"

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The fonts the roles name until a game says otherwise, and their sizes in
 * logical pixels to the em: the author's choices, and easy to change.
 * Change a line here to change a default; a family that is not in
 * TB_FONT_DEFAULT_FAMILIES (cmake/EmbedFonts.cmake) is then missing from
 * builds with TB_EMBED_FONTS=defaults, so change that list too. No role is
 * bold: a caller that wants bold asks for it (tb_font_spec_bold()). The
 * pixel role's size is its font's design size, at which one pixel of the
 * font is one pixel of a display at 100%. */
static const tb_font_role_choice tb_font_default_roles[TB_FONT_ROLE_COUNT] = {
    [TB_FONT_ROLE_UI] = {"Source Sans 3", TB_FONT_WEIGHT_REGULAR, false, 16.0f},
    [TB_FONT_ROLE_BODY] = {"Source Serif 4", TB_FONT_WEIGHT_REGULAR, false, 18.0f},
    [TB_FONT_ROLE_HEADING] = {"Source Sans 3", TB_FONT_WEIGHT_REGULAR, false, 28.0f},
    [TB_FONT_ROLE_MONOSPACE] = {"JetBrains Mono", TB_FONT_WEIGHT_REGULAR, false, 15.0f},
    [TB_FONT_ROLE_PIXEL] = {"Px437 IBM VGA 8x16", TB_FONT_WEIGHT_REGULAR, false, 16.0f},
};

/* Ranks of a weight against the weight asked for: lower is better. */
#define TB_FONT_RANK_CLASS ((uint32_t)0x10000)

/* The weights CSS tries first when asked for one from 400 to 500. */
#define TB_FONT_WEIGHT_MIDDLE_LOW 400
#define TB_FONT_WEIGHT_MIDDLE_HIGH 500

/* A font of the catalog. An added font owns its bytes and names. */
typedef struct tb_font_catalog_entry
{
    tb_font_info info;
    size_t embedded_index; /* For tb_font_embedded_get_bytes(); unused when added. */
    void *bytes;           /* An added font's copy of its file; NULL when embedded. */
    char *family;          /* An added font's family; NULL when embedded. */
    char *style;           /* An added font's style; NULL when embedded. */
} tb_font_catalog_entry;

struct tb_font_catalog
{
    tb_allocator allocator;
    tb_array *entries; /* Of tb_font_catalog_entry. */
    tb_font_role_choice roles[TB_FONT_ROLE_COUNT];
    char *role_families[TB_FONT_ROLE_COUNT]; /* The roles' families, owned. */
};

tb_font_role_choice tb_font_default_role(tb_font_role role)
{
    TB_ASSERT_MSG((size_t)role < TB_FONT_ROLE_COUNT, "a role");
    return tb_font_default_roles[role];
}

static char *tb_font_catalog_copy_text(const tb_allocator *allocator, const char *text,
                                       size_t length)
{
    /* `length + 1` bytes of the text exist, its NUL the last, so the sum cannot overflow. */
    char *copy = tb_allocator_calloc(allocator, length + 1, 1, 1);
    memcpy(copy, text, length);
    return copy;
}

static void tb_font_catalog_free_text(const tb_allocator *allocator, char *text)
{
    if (text != NULL)
    {
        tb_allocator_free(allocator, text, tb_str_length(text) + 1, 1);
    }
}

tb_font_catalog *tb_font_catalog_create(const tb_allocator *allocator)
{
    tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_font_catalog *catalog = TB_ALLOCATOR_NEW(&resolved, tb_font_catalog);
    size_t embedded = tb_font_embedded_count();
    catalog->allocator = resolved;
    catalog->entries =
        tb_array_create(&resolved, sizeof(tb_font_catalog_entry), _Alignof(tb_font_catalog_entry));
    tb_array_reserve(catalog->entries, embedded);
    for (size_t index = 0; index < embedded; index++)
    {
        tb_font_catalog_entry entry = {0};
        tb_font_embedded_get_info(index, &entry.info);
        entry.embedded_index = index;
        tb_array_push(catalog->entries, &entry);
    }
    for (size_t role = 0; role < TB_FONT_ROLE_COUNT; role++)
    {
        tb_font_catalog_set_role(catalog, (tb_font_role)role, &tb_font_default_roles[role]);
    }
    return catalog;
}

void tb_font_catalog_destroy(tb_font_catalog *catalog)
{
    tb_allocator allocator = {0};
    if (catalog == NULL)
    {
        return;
    }
    allocator = catalog->allocator;
    for (size_t index = 0; index < tb_array_length(catalog->entries); index++)
    {
        tb_font_catalog_entry *entry = tb_array_at(catalog->entries, index);
        if (entry->bytes != NULL)
        {
            tb_allocator_free(&allocator, entry->bytes, entry->info.length, 1);
        }
        tb_font_catalog_free_text(&allocator, entry->family);
        tb_font_catalog_free_text(&allocator, entry->style);
    }
    for (size_t role = 0; role < TB_FONT_ROLE_COUNT; role++)
    {
        tb_font_catalog_free_text(&allocator, catalog->role_families[role]);
    }
    tb_array_destroy(catalog->entries);
    TB_ALLOCATOR_DELETE(&allocator, tb_font_catalog, catalog);
}

size_t tb_font_catalog_count(const tb_font_catalog *catalog)
{
    TB_ASSERT(catalog != NULL);
    return tb_array_length(catalog->entries);
}

static const tb_font_catalog_entry *tb_font_catalog_entry_at(const tb_font_catalog *catalog,
                                                             size_t index)
{
    TB_ASSERT(catalog != NULL);
    TB_ASSERT_MSG(index < tb_array_length(catalog->entries),
                  "the index is less than tb_font_catalog_count()");
    return tb_array_at(catalog->entries, index);
}

void tb_font_catalog_get_info(const tb_font_catalog *catalog, size_t index, tb_font_info *info)
{
    TB_ASSERT(info != NULL);
    *info = tb_font_catalog_entry_at(catalog, index)->info;
}

void tb_font_catalog_get_bytes(const tb_font_catalog *catalog, size_t index, const void **bytes,
                               size_t *length)
{
    const tb_font_catalog_entry *entry = tb_font_catalog_entry_at(catalog, index);
    TB_ASSERT(bytes != NULL);
    TB_ASSERT(length != NULL);
    if (entry->info.embedded)
    {
        tb_font_embedded_get_bytes(entry->embedded_index, bytes, length);
        return;
    }
    *bytes = entry->bytes;
    *length = entry->info.length;
}

static char tb_font_ascii_lower(char character)
{
    if (character >= 'A' && character <= 'Z')
    {
        return (char)(character - 'A' + 'a');
    }
    return character;
}

static bool tb_font_family_equal(const char *left, const char *right)
{
    size_t index = 0;
    while (left[index] != '\0' && right[index] != '\0')
    {
        if (tb_font_ascii_lower(left[index]) != tb_font_ascii_lower(right[index]))
        {
            return false;
        }
        index++;
    }
    return left[index] == right[index];
}

/* How well a weight matches the one asked for, as CSS ranks it: lower is
 * better. The class says which group of weights CSS tries it in; the
 * distance, how far it is within the group. */
static uint32_t tb_font_weight_rank(uint16_t wanted, uint16_t weight)
{
    uint32_t distance = weight > wanted ? (uint32_t)(weight - wanted) : (uint32_t)(wanted - weight);
    uint32_t group = 0;
    bool heavier = weight >= wanted;
    if (wanted >= TB_FONT_WEIGHT_MIDDLE_LOW && wanted <= TB_FONT_WEIGHT_MIDDLE_HIGH)
    {
        if (heavier && weight <= TB_FONT_WEIGHT_MIDDLE_HIGH)
        {
            group = 0;
        }
        else if (!heavier)
        {
            group = 1;
        }
        else
        {
            group = 2;
        }
    }
    else if (wanted < TB_FONT_WEIGHT_MIDDLE_LOW)
    {
        group = heavier && weight != wanted ? 1 : 0;
    }
    else
    {
        group = heavier ? 0 : 1;
    }
    return group * TB_FONT_RANK_CLASS + distance;
}

bool tb_font_catalog_find(const tb_font_catalog *catalog, const char *family, uint16_t weight,
                          bool italic, size_t *index)
{
    bool found = false;
    bool best_slant_matches = false;
    uint32_t best_rank = 0;
    size_t best = 0;
    TB_ASSERT(catalog != NULL);
    TB_ASSERT(family != NULL);
    TB_ASSERT(index != NULL);
    for (size_t candidate = 0; candidate < tb_array_length(catalog->entries); candidate++)
    {
        const tb_font_catalog_entry *entry = tb_array_at(catalog->entries, candidate);
        bool slant_matches = entry->info.italic == italic;
        uint32_t rank = tb_font_weight_rank(weight, entry->info.weight);
        bool better = false;
        if (!tb_font_family_equal(entry->info.family, family))
        {
            continue;
        }
        /* A later font wins a tie, so that a game's font replaces ours. */
        if (!found || slant_matches != best_slant_matches)
        {
            better = !found || slant_matches;
        }
        else
        {
            better = rank <= best_rank;
        }
        if (better)
        {
            found = true;
            best_slant_matches = slant_matches;
            best_rank = rank;
            best = candidate;
        }
    }
    if (found)
    {
        *index = best;
    }
    return found;
}

tb_font_result tb_font_catalog_add(tb_font_catalog *catalog, const void *bytes, size_t length,
                                   tb_font_category category, size_t *index, tb_font_error *error)
{
    tb_font_catalog_entry entry = {0};
    tb_font_error failure = {0};
    tb_font *font = NULL;
    tb_font_style style = {0};
    tb_font_result result = TB_FONT_OK;
    TB_ASSERT(catalog != NULL);
    TB_ASSERT(bytes != NULL);

    entry.bytes = tb_allocator_alloc(&catalog->allocator, length > 0 ? length : 1, 1);
    memcpy(entry.bytes, bytes, length);
    result = tb_font_open(entry.bytes, length, &catalog->allocator, &font, &failure);
    if (result != TB_FONT_OK)
    {
        goto failed;
    }
    tb_font_get_style(font, &style);
    if (style.family_length == 0)
    {
        result = TB_FONT_MALFORMED;
        failure = (tb_font_error){TB_FONT_MALFORMED, "name", "the font names no family"};
        goto failed;
    }
    entry.family =
        tb_font_catalog_copy_text(&catalog->allocator, style.family, style.family_length);
    entry.style = tb_font_catalog_copy_text(&catalog->allocator, style.style, style.style_length);
    entry.info = (tb_font_info){
        .family = entry.family,
        .style = entry.style,
        .weight = style.weight,
        .italic = style.italic,
        .category = category,
        .license = TB_FONT_LICENSE_GAME,
        .file = "",
        .embedded = false,
        .pixel_size = 0,
        .length = length,
    };
    tb_font_close(font);
    if (index != NULL)
    {
        *index = tb_array_length(catalog->entries);
    }
    tb_array_push(catalog->entries, &entry);
    if (error != NULL)
    {
        *error = failure;
    }
    return TB_FONT_OK;

failed:
    tb_font_close(font);
    tb_allocator_free(&catalog->allocator, entry.bytes, length > 0 ? length : 1, 1);
    if (error != NULL)
    {
        *error = failure;
    }
    return result;
}

void tb_font_catalog_set_role(tb_font_catalog *catalog, tb_font_role role,
                              const tb_font_role_choice *choice)
{
    char *family = NULL;
    TB_ASSERT(catalog != NULL);
    TB_ASSERT_MSG((size_t)role < TB_FONT_ROLE_COUNT, "a role");
    TB_ASSERT(choice != NULL);
    TB_ASSERT(choice->family != NULL);
    TB_ASSERT_MSG(choice->size >= 0.0f && choice->size <= FLT_MAX,
                  "a role's size is 0, for its default, or a finite size");
    family = tb_font_catalog_copy_text(&catalog->allocator, choice->family,
                                       tb_str_length(choice->family));
    tb_font_catalog_free_text(&catalog->allocator, catalog->role_families[role]);
    catalog->role_families[role] = family;
    catalog->roles[role] = (tb_font_role_choice){
        family, choice->weight, choice->italic,
        choice->size > 0.0f ? choice->size : tb_font_default_roles[role].size};
}

tb_font_role_choice tb_font_catalog_get_role(const tb_font_catalog *catalog, tb_font_role role)
{
    TB_ASSERT(catalog != NULL);
    TB_ASSERT_MSG((size_t)role < TB_FONT_ROLE_COUNT, "a role");
    return catalog->roles[role];
}

bool tb_font_catalog_find_role(const tb_font_catalog *catalog, tb_font_role role, size_t *index)
{
    tb_font_role_choice choice = tb_font_catalog_get_role(catalog, role);
    return tb_font_catalog_find(catalog, choice.family, choice.weight, choice.italic, index);
}

bool tb_font_catalog_open(const tb_font_catalog *catalog, size_t index,
                          const tb_allocator *allocator, tb_font **font)
{
    const void *bytes = NULL;
    size_t length = 0;

    TB_ASSERT(font != NULL);
    tb_font_catalog_get_bytes(catalog, index, &bytes, &length);
    if (tb_font_open(bytes, length, allocator, font, NULL) != TB_FONT_OK)
    {
        /* coverage-off: every font of a catalog opened once before */
        *font = NULL;
        return false;
        /* coverage-on */
    }
    return true;
}

tb_font_spec tb_font_spec_of_role(const tb_font_catalog *catalog, tb_font_role role)
{
    const tb_font_role_choice choice = tb_font_catalog_get_role(catalog, role);
    return (tb_font_spec){role, choice.weight, choice.italic, choice.size};
}

tb_font_spec tb_font_spec_bold(tb_font_spec spec)
{
    spec.weight = TB_FONT_WEIGHT_BOLD;
    return spec;
}

tb_font_spec tb_font_spec_italic(tb_font_spec spec)
{
    spec.italic = true;
    return spec;
}

tb_font_spec tb_font_spec_sized(tb_font_spec spec, float size)
{
    TB_ASSERT_MSG(size > 0.0f && size <= FLT_MAX, "a spec of a finite size");
    spec.size = size;
    return spec;
}

bool tb_font_catalog_find_spec(const tb_font_catalog *catalog, tb_font_spec spec, size_t *index)
{
    const tb_font_role_choice choice = tb_font_catalog_get_role(catalog, spec.role);
    return tb_font_catalog_find(catalog, choice.family, spec.weight, spec.italic, index);
}

const char *tb_font_role_name(tb_font_role role)
{
    switch (role)
    {
    case TB_FONT_ROLE_UI:
        return "ui";
    case TB_FONT_ROLE_BODY:
        return "body";
    case TB_FONT_ROLE_HEADING:
        return "heading";
    case TB_FONT_ROLE_MONOSPACE:
        return "monospace";
    case TB_FONT_ROLE_PIXEL:
        return "pixel";
    case TB_FONT_ROLE_COUNT:
        break;
    }
    return "unknown";
}
