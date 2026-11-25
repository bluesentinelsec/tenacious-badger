/* Golden images: see golden.h. */
#include "golden.h"

#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/memory.h"
#include "badger/str.h"

#include "tb_test.h"
#include "test_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern const tb_test_file tb_golden_files[];
extern const size_t tb_golden_files_count;

/* How far a pixel's channel may stray from its golden image's before it
 * counts as different, and how many pixels in a thousand may differ:
 * WARP's results move a little between versions of Windows. */
#define GOLDEN_CHANNEL_TOLERANCE 16
#define GOLDEN_DIFFERENT_PER_MILLE 10u
/* Bytes of the headers of the BMP files the golden images are. */
#define BMP_FILE_HEADER 14u
#define BMP_INFO_HEADER 40u

tb_window *golden_window(tb_window_system *system, const char *title)
{
    tb_window *window = NULL;
    tb_window_state state = {0};
    tb_window_options options = {
        .title = title, .width = GOLDEN_WIDTH, .height = GOLDEN_HEIGHT, .fixed_size = true};

    TB_TEST_REQUIRE_UINT(tb_window_create(system, &options, &window, NULL), ==, TB_WINDOW_OK);
    tb_window_state_of(window, &state);
    if (state.pixel_width == GOLDEN_WIDTH && state.pixel_height == GOLDEN_HEIGHT)
    {
        return window;
    }
    tb_window_destroy(window);
    options.width = (uint32_t)lround(GOLDEN_WIDTH / state.content_scale);
    options.height = (uint32_t)lround(GOLDEN_HEIGHT / state.content_scale);
    TB_TEST_REQUIRE_UINT(tb_window_create(system, &options, &window, NULL), ==, TB_WINDOW_OK);
    tb_window_state_of(window, &state);
    if (state.pixel_width != GOLDEN_WIDTH || state.pixel_height != GOLDEN_HEIGHT)
    {
        tb_window_destroy(window);
        tb_window_system_close(system);
        TB_TEST_SKIP("no window of %ux%u pixels at a display scale of %.2f", GOLDEN_WIDTH,
                     GOLDEN_HEIGHT, state.content_scale);
    }
    return window;
}

static void write_u16(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *at, uint32_t value)
{
    write_u16(at, value & 0xFFFFu);
    write_u16(at + 2, value >> 16);
}

static uint32_t read_u32(const uint8_t *at)
{
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

/* Write a frame as a 32-bit BMP, rows from the top, so that it can become
 * a golden image: `<name>.actual.bmp` in the working directory. */
static void write_actual(const char *name, const uint8_t *pixels, uint32_t width, uint32_t height)
{
    const size_t pixel_bytes = (size_t)width * height * 4u;
    const size_t size = BMP_FILE_HEADER + BMP_INFO_HEADER + pixel_bytes;
    uint8_t *const file = tb_calloc(1, size);
    char path[128] = {0};

    file[0] = 'B';
    file[1] = 'M';
    write_u32(&file[2], (uint32_t)size);
    write_u32(&file[10], BMP_FILE_HEADER + BMP_INFO_HEADER);
    write_u32(&file[14], BMP_INFO_HEADER);
    write_u32(&file[18], width);
    write_u32(&file[22], (uint32_t)-(int32_t)height); /* negative: rows from the top */
    write_u16(&file[26], 1);
    write_u16(&file[28], 32);
    write_u32(&file[34], (uint32_t)pixel_bytes);
    memcpy(&file[BMP_FILE_HEADER + BMP_INFO_HEADER], pixels, pixel_bytes);
    (void)tb_fmt_buffer(path, sizeof(path), "%s.actual.bmp", name);
    (void)tb_fs_write_file((tb_path_view){path, tb_str_length(path)}, file, size);
    tb_free(file);
}

/* The pixels of a golden image written as write_actual() writes, or NULL
 * when there is none of that name and size. */
static const uint8_t *golden_pixels(const char *name, uint32_t width, uint32_t height)
{
    char file_name[128] = {0};

    (void)tb_fmt_buffer(file_name, sizeof(file_name), "%s.bmp", name);
    for (size_t index = 0; index < tb_golden_files_count; index++)
    {
        const tb_test_file *const file = &tb_golden_files[index];
        if (!tb_str_equal(file->name, file_name))
        {
            continue;
        }
        if (file->length != BMP_FILE_HEADER + BMP_INFO_HEADER + (size_t)width * height * 4u ||
            read_u32(&file->bytes[18]) != width ||
            read_u32(&file->bytes[22]) != (uint32_t)-(int32_t)height)
        {
            return NULL;
        }
        return &file->bytes[BMP_FILE_HEADER + BMP_INFO_HEADER];
    }
    return NULL;
}

/* Compare a frame with its golden image, within the tolerances; on a
 * difference, or without one, write the frame beside the tests, to look at
 * or to take as the golden image (tests/data/golden). */
void expect_golden(const char *name, const uint8_t *pixels, uint32_t width, uint32_t height)
{
    const uint8_t *const golden = golden_pixels(name, width, height);
    size_t different = 0;

    if (golden == NULL)
    {
        write_actual(name, pixels, width, height);
        TB_TEST_FAIL("no golden image %s.bmp of %ux%u; this frame is in %s.actual.bmp", name, width,
                     height, name);
        return;
    }
    for (size_t pixel = 0; pixel < (size_t)width * height; pixel++)
    {
        for (size_t channel = 0; channel < 3; channel++)
        {
            const int actual = pixels[pixel * 4u + channel];
            const int expected = golden[pixel * 4u + channel];
            if (abs(actual - expected) > GOLDEN_CHANNEL_TOLERANCE)
            {
                different++;
                break;
            }
        }
    }
    TB_TEST_CONTEXT("%s: %zu pixels differ", name, different);
    if (different * 1000u > (size_t)width * height * GOLDEN_DIFFERENT_PER_MILLE)
    {
        write_actual(name, pixels, width, height);
        TB_TEST_FAIL("%s differs from its golden image in %zu pixels; this frame is in "
                     "%s.actual.bmp",
                     name, different, name);
    }
}
