/* The fonts the engine carries: what each file of resources/fonts/ is,
 * and its bytes, decompressed the first time they are asked for. */
#include "font_embedded.h"
#include "badger/assert.h"
#include "badger/deflate.h"
#include "badger/fatal.h"
#include "badger/font_catalog.h"
#include "badger/size.h"
#include "badger/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a file of the pack is, as its own tables say (the tests check that
 * tb_font_get_style() agrees), and what the pack's README says of it. */
typedef struct tb_font_embedded_file
{
    const char *file;
    const char *family;
    const char *style;
    uint16_t weight;
    bool italic;
    tb_font_category category;
    tb_font_license license;
    uint16_t pixel_size; /* See tb_font_info::pixel_size. */
} tb_font_embedded_file;

/* Every file of resources/fonts/, in the order of their names, as the
 * generated index has them. A font added to the pack is added here too. */
static const tb_font_embedded_file tb_font_embedded_files[] = {
    {"mono/FiraCode-Bold.ttf", "Fira Code", "Bold", 700, false, TB_FONT_CATEGORY_MONO,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/FiraCode-Regular.ttf", "Fira Code", "Regular", 400, false, TB_FONT_CATEGORY_MONO,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/FiraCode-SemiBold.ttf", "Fira Code", "SemiBold", 600, false, TB_FONT_CATEGORY_MONO,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/IBMPlexMono-Bold.ttf", "IBM Plex Mono", "Bold", 700, false, TB_FONT_CATEGORY_MONO,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/IBMPlexMono-BoldItalic.ttf", "IBM Plex Mono", "Bold Italic", 700, true,
     TB_FONT_CATEGORY_MONO, TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/IBMPlexMono-Italic.ttf", "IBM Plex Mono", "Italic", 400, true, TB_FONT_CATEGORY_MONO,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/IBMPlexMono-Regular.ttf", "IBM Plex Mono", "Regular", 400, false, TB_FONT_CATEGORY_MONO,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/JetBrainsMono-Bold.ttf", "JetBrains Mono", "Bold", 700, false, TB_FONT_CATEGORY_MONO,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/JetBrainsMono-BoldItalic.ttf", "JetBrains Mono", "Bold Italic", 700, true,
     TB_FONT_CATEGORY_MONO, TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/JetBrainsMono-Italic.ttf", "JetBrains Mono", "Italic", 400, true, TB_FONT_CATEGORY_MONO,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/JetBrainsMono-Regular.ttf", "JetBrains Mono", "Regular", 400, false,
     TB_FONT_CATEGORY_MONO, TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/JetBrainsMono-SemiBold.ttf", "JetBrains Mono", "SemiBold", 600, false,
     TB_FONT_CATEGORY_MONO, TB_FONT_LICENSE_OFL_1_1, 0},
    {"mono/JetBrainsMono-SemiBoldItalic.ttf", "JetBrains Mono", "SemiBold Italic", 600, true,
     TB_FONT_CATEGORY_MONO, TB_FONT_LICENSE_OFL_1_1, 0},
    {"retro/Monocraft-Bold.ttf", "Monocraft", "Bold", 700, false, TB_FONT_CATEGORY_RETRO,
     TB_FONT_LICENSE_OFL_1_1, 9},
    {"retro/Monocraft-BoldItalic.ttf", "Monocraft", "Bold Italic", 700, true,
     TB_FONT_CATEGORY_RETRO, TB_FONT_LICENSE_OFL_1_1, 0},
    {"retro/Monocraft-Italic.ttf", "Monocraft", "Italic", 400, true, TB_FONT_CATEGORY_RETRO,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"retro/Monocraft-SemiBold.ttf", "Monocraft", "SemiBold", 600, false, TB_FONT_CATEGORY_RETRO,
     TB_FONT_LICENSE_OFL_1_1, 9},
    {"retro/Monocraft-SemiBoldItalic.ttf", "Monocraft", "SemiBold Italic", 600, true,
     TB_FONT_CATEGORY_RETRO, TB_FONT_LICENSE_OFL_1_1, 0},
    {"retro/Monocraft.ttf", "Monocraft", "Regular", 400, false, TB_FONT_CATEGORY_RETRO,
     TB_FONT_LICENSE_OFL_1_1, 9},
    {"retro/PressStart2P-Regular.ttf", "Press Start 2P", "Regular", 400, false,
     TB_FONT_CATEGORY_RETRO, TB_FONT_LICENSE_OFL_1_1, 8},
    {"retro/Px437_IBM_VGA_8x16.ttf", "Px437 IBM VGA 8x16", "Regular", 400, false,
     TB_FONT_CATEGORY_RETRO, TB_FONT_LICENSE_CC_BY_SA_4_0, 16},
    {"retro/Silkscreen-Bold.ttf", "Silkscreen", "Bold", 700, false, TB_FONT_CATEGORY_RETRO,
     TB_FONT_LICENSE_OFL_1_1, 8},
    {"retro/Silkscreen-Regular.ttf", "Silkscreen", "Regular", 400, false, TB_FONT_CATEGORY_RETRO,
     TB_FONT_LICENSE_OFL_1_1, 8},
    {"retro/VT323-Regular.ttf", "VT323", "Regular", 400, false, TB_FONT_CATEGORY_RETRO,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Inter-Bold.ttf", "Inter", "Bold", 700, false, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Inter-BoldItalic.ttf", "Inter", "Bold Italic", 700, true, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Inter-Italic.ttf", "Inter", "Italic", 400, true, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Inter-Regular.ttf", "Inter", "Regular", 400, false, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Inter-SemiBold.ttf", "Inter", "SemiBold", 600, false, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Inter-SemiBoldItalic.ttf", "Inter", "SemiBold Italic", 600, true, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Roboto-Bold.ttf", "Roboto", "Bold", 700, false, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Roboto-BoldItalic.ttf", "Roboto", "Bold Italic", 700, true, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Roboto-Italic.ttf", "Roboto", "Italic", 400, true, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Roboto-Regular.ttf", "Roboto", "Regular", 400, false, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Roboto-SemiBold.ttf", "Roboto", "SemiBold", 600, false, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/Roboto-SemiBoldItalic.ttf", "Roboto", "SemiBold Italic", 600, true,
     TB_FONT_CATEGORY_SANS, TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/SourceSans3-Bold.ttf", "Source Sans 3", "Bold", 700, false, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/SourceSans3-BoldItalic.ttf", "Source Sans 3", "Bold Italic", 700, true,
     TB_FONT_CATEGORY_SANS, TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/SourceSans3-Italic.ttf", "Source Sans 3", "Italic", 400, true, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/SourceSans3-Regular.ttf", "Source Sans 3", "Regular", 400, false, TB_FONT_CATEGORY_SANS,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/SourceSans3-SemiBold.ttf", "Source Sans 3", "SemiBold", 600, false,
     TB_FONT_CATEGORY_SANS, TB_FONT_LICENSE_OFL_1_1, 0},
    {"sans/SourceSans3-SemiBoldItalic.ttf", "Source Sans 3", "SemiBold Italic", 600, true,
     TB_FONT_CATEGORY_SANS, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/EBGaramond-Bold.ttf", "EB Garamond", "Bold", 700, false, TB_FONT_CATEGORY_SERIF,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/EBGaramond-BoldItalic.ttf", "EB Garamond", "Bold Italic", 700, true,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/EBGaramond-Italic.ttf", "EB Garamond", "Italic", 400, true, TB_FONT_CATEGORY_SERIF,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/EBGaramond-Regular.ttf", "EB Garamond", "Regular", 400, false, TB_FONT_CATEGORY_SERIF,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/EBGaramond-SemiBold.ttf", "EB Garamond", "SemiBold", 600, false, TB_FONT_CATEGORY_SERIF,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/EBGaramond-SemiBoldItalic.ttf", "EB Garamond", "SemiBold Italic", 600, true,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/LibreBaskerville-Bold.ttf", "Libre Baskerville", "Bold", 700, false,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/LibreBaskerville-BoldItalic.ttf", "Libre Baskerville", "Bold Italic", 700, true,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/LibreBaskerville-Italic.ttf", "Libre Baskerville", "Italic", 400, true,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/LibreBaskerville-Regular.ttf", "Libre Baskerville", "Regular", 400, false,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/LibreBaskerville-SemiBold.ttf", "Libre Baskerville", "SemiBold", 600, false,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/LibreBaskerville-SemiBoldItalic.ttf", "Libre Baskerville", "SemiBold Italic", 600, true,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/SourceSerif4-Bold.ttf", "Source Serif 4", "Bold", 700, false, TB_FONT_CATEGORY_SERIF,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/SourceSerif4-BoldItalic.ttf", "Source Serif 4", "Bold Italic", 700, true,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/SourceSerif4-Italic.ttf", "Source Serif 4", "Italic", 400, true, TB_FONT_CATEGORY_SERIF,
     TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/SourceSerif4-Regular.ttf", "Source Serif 4", "Regular", 400, false,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/SourceSerif4-SemiBold.ttf", "Source Serif 4", "SemiBold", 600, false,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
    {"serif/SourceSerif4-SemiBoldItalic.ttf", "Source Serif 4", "SemiBold Italic", 600, true,
     TB_FONT_CATEGORY_SERIF, TB_FONT_LICENSE_OFL_1_1, 0},
};

TB_STATIC_ASSERT(sizeof tb_font_embedded_files / sizeof tb_font_embedded_files[0] ==
                     TB_FONT_EMBEDDED_FILE_COUNT,
                 "TB_FONT_EMBEDDED_FILE_COUNT counts the files described");

/* The process's fonts, decompressed once each and never released. */
static tb_font_embedded_cache tb_font_embedded_process_cache;

/* Files of the index that the table can describe; they agree unless a font
 * was added to the pack and not to the table, which the tests catch. */
static size_t tb_font_embedded_described_count(void)
{
    TB_ASSERT_MSG(tb_font_embedded_index_count == TB_FONT_EMBEDDED_FILE_COUNT,
                  "every font of resources/fonts/ is described in font_embedded.c");
    if (tb_font_embedded_index_count < TB_FONT_EMBEDDED_FILE_COUNT)
    {
        return tb_font_embedded_index_count;
    }
    return TB_FONT_EMBEDDED_FILE_COUNT;
}

/* The slot of the pack that holds the embedded font of an index. */
static size_t tb_font_embedded_slot(size_t index)
{
    size_t described = tb_font_embedded_described_count();
    size_t found = described;
    size_t seen = 0;
    for (size_t slot = 0; slot < described && found == described; slot++)
    {
        if (tb_font_embedded_index[slot].blob == NULL)
        {
            continue;
        }
        if (seen == index)
        {
            found = slot;
        }
        seen++;
    }
    TB_ASSERT_MSG(found < described, "the index is less than tb_font_embedded_count()");
    TB_ASSERT_MSG(
        tb_str_equal(tb_font_embedded_index[found].file, tb_font_embedded_files[found].file),
        "font_embedded.c describes the pack's files in the order of their names");
    return found;
}

size_t tb_font_embedded_count(void)
{
    size_t described = tb_font_embedded_described_count();
    size_t count = 0;
    for (size_t slot = 0; slot < described; slot++)
    {
        if (tb_font_embedded_index[slot].blob != NULL)
        {
            count++;
        }
    }
    return count;
}

void tb_font_embedded_get_info(size_t index, tb_font_info *info)
{
    size_t slot = 0;
    const tb_font_embedded_file *file = NULL;
    TB_ASSERT(info != NULL);
    slot = tb_font_embedded_slot(index);
    file = &tb_font_embedded_files[slot];
    *info = (tb_font_info){
        .family = file->family,
        .style = file->style,
        .weight = file->weight,
        .italic = file->italic,
        .category = file->category,
        .license = file->license,
        .file = file->file,
        .embedded = true,
        .pixel_size = file->pixel_size,
        .length = tb_font_embedded_index[slot].blob->length,
    };
}

void tb_font_embedded_get_bytes(size_t index, const void **bytes, size_t *length)
{
    size_t slot = 0;
    const tb_font_blob *blob = NULL;
    TB_ASSERT(bytes != NULL);
    TB_ASSERT(length != NULL);
    slot = tb_font_embedded_slot(index);
    blob = tb_font_embedded_index[slot].blob;
    *bytes = tb_font_embedded_cache_get(&tb_font_embedded_process_cache, slot, blob);
    *length = blob->length;
}

/* Decompress a font into pages of its own, which `region` then owns. */
static void *tb_font_embedded_decompress(tb_vm_region *region, const tb_font_blob *blob)
{
    size_t page_size = tb_vm_get_info().page_size;
    size_t rounded = 0;
    size_t written = 0;
    tb_vm_result reserved = TB_VM_OK;
    tb_vm_result committed = TB_VM_OK;
    tb_deflate_result result = TB_DEFLATE_OK;

    TB_ASSERT(blob->length > 0);
    /* coverage-off: the system reports no page size, or a font fills the address space */
    if (page_size == 0 || !tb_size_add(blob->length, page_size - 1, &rounded))
    {
        tb_fatal("fonts: no room for a font of %zu bytes", blob->length);
    }
    /* coverage-on */
    rounded -= rounded % page_size;
    reserved = tb_vm_reserve(rounded, region);
    committed = reserved == TB_VM_OK ? tb_vm_commit(region, 0, rounded) : reserved;
    /* coverage-off: the system is out of memory */
    if (committed != TB_VM_OK)
    {
        tb_fatal("fonts: cannot get %zu bytes for a font: %s", rounded,
                 tb_vm_result_name(committed));
    }
    /* coverage-on */
    result = tb_inflate(TB_DEFLATE_ZLIB, blob->data, blob->data_length, region->address,
                        blob->length, &written);
    /* coverage-off: the build checked the stream when it made it */
    if (result != TB_DEFLATE_OK || written != blob->length)
    {
        tb_fatal("fonts: an embedded font does not decompress: %s", tb_deflate_result_name(result));
    }
    /* coverage-on */
    return region->address;
}

const void *tb_font_embedded_cache_get(tb_font_embedded_cache *cache, size_t slot,
                                       const tb_font_blob *blob)
{
    void *bytes = NULL;
    TB_ASSERT(cache != NULL);
    TB_ASSERT(blob != NULL);
    TB_ASSERT(slot < TB_FONT_EMBEDDED_FILE_COUNT);
    bytes = tb_platform_atomic_pointer_load(&cache->bytes[slot]);
    if (bytes != NULL)
    {
        return bytes;
    }
    tb_mutex_lock(&cache->mutex);
    bytes = tb_platform_atomic_pointer_load(&cache->bytes[slot]);
    if (bytes == NULL)
    {
        bytes = tb_font_embedded_decompress(&cache->regions[slot], blob);
        tb_platform_atomic_pointer_store(&cache->bytes[slot], bytes);
    }
    tb_mutex_unlock(&cache->mutex);
    return bytes;
}

void tb_font_embedded_cache_release(tb_font_embedded_cache *cache)
{
    TB_ASSERT(cache != NULL);
    for (size_t slot = 0; slot < TB_FONT_EMBEDDED_FILE_COUNT; slot++)
    {
        tb_vm_result released = tb_vm_release(&cache->regions[slot]);
        TB_ASSERT_MSG(released == TB_VM_OK, "a font's pages are released");
        (void)released;
        tb_platform_atomic_pointer_store(&cache->bytes[slot], NULL);
    }
}

const char *tb_font_category_name(tb_font_category category)
{
    switch (category)
    {
    case TB_FONT_CATEGORY_SERIF:
        return "serif";
    case TB_FONT_CATEGORY_SANS:
        return "sans";
    case TB_FONT_CATEGORY_MONO:
        return "mono";
    case TB_FONT_CATEGORY_RETRO:
        return "retro";
    case TB_FONT_CATEGORY_OTHER:
        return "other";
    }
    return "unknown";
}

const char *tb_font_license_name(tb_font_license license)
{
    switch (license)
    {
    case TB_FONT_LICENSE_OFL_1_1:
        return "OFL-1.1";
    case TB_FONT_LICENSE_CC_BY_SA_4_0:
        return "CC-BY-SA-4.0";
    case TB_FONT_LICENSE_GAME:
        return "game";
    }
    return "unknown";
}
