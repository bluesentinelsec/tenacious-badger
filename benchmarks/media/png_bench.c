/* PNG files: decoding a large RGBA image of smooth gradients and noise, as
 * art and screenshots are, against what it is made of: decompressing its
 * image data alone, and decoding the same pixels from a BMP file, which is
 * copying them. Encoding is timed too. Operations are pixels, so the
 * numbers are nanoseconds per pixel. */
#include "tb_bench.h"

#include "badger/bitmap.h"
#include "badger/deflate.h"
#include "badger/memory.h"
#include "badger/stream.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Pixels across and down of the image. */
#define PNG_BENCH_SIDE ((size_t)2048)

typedef struct png_bench_state
{
    tb_bitmap *bitmap;
    const uint8_t *png;
    size_t png_length;
    const uint8_t *bmp;
    size_t bmp_length;
    uint8_t *idat; /* The image data of the PNG file, joined. */
    size_t idat_length;
    uint8_t *rows; /* Room for the filtered rows the image data holds. */
    size_t rows_length;
} png_bench_state;

static void run_decode(void *context, const uint8_t *bytes, size_t length)
{
    (void)context;
    tb_bitmap *bitmap = NULL;
    (void)tb_bitmap_decode(bytes, length, NULL, NULL, &bitmap, NULL);
    tb_bench_keep(tb_bitmap_get_pixel(bitmap, 1, 1).r);
    tb_bitmap_destroy(bitmap);
}

static void run_decode_png(void *context)
{
    const png_bench_state *const state = context;
    run_decode(context, state->png, state->png_length);
}

static void run_decode_bmp(void *context)
{
    const png_bench_state *const state = context;
    run_decode(context, state->bmp, state->bmp_length);
}

static void run_inflate(void *context)
{
    const png_bench_state *const state = context;
    size_t written = 0;
    (void)tb_inflate(TB_DEFLATE_ZLIB, state->idat, state->idat_length, state->rows,
                     state->rows_length, &written);
    tb_bench_keep(written);
}

static void run_encode(void *context)
{
    const png_bench_state *const state = context;
    tb_stream sink = {0};
    size_t length = 0;
    tb_stream_open_buffer(NULL, &sink);
    (void)tb_bitmap_write_png(state->bitmap, NULL, &sink, NULL);
    (void)tb_stream_buffer_bytes(&sink, &length);
    tb_bench_keep(length);
    (void)tb_stream_close(&sink);
}

static uint32_t png_bench_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

/* Join the data of the IDAT chunks of a file the writer wrote. */
static void png_bench_join_idat(png_bench_state *state)
{
    size_t at = 8;

    state->idat = tb_malloc(state->png_length);
    while (at + 12u <= state->png_length)
    {
        const size_t length = png_bench_u32(state->png + at);
        if (memcmp(state->png + at + 4u, "IDAT", 4) == 0)
        {
            memcpy(state->idat + state->idat_length, state->png + at + 8u, length);
            state->idat_length += length;
        }
        at += 12u + length;
    }
}

/* Gradients, a few hard edges and some noise, half transparent in places. */
static void png_bench_paint(tb_bitmap *bitmap, size_t side)
{
    uint64_t random = 0x9E3779B97F4A7C15u;

    for (size_t y = 0; y < side; y++)
    {
        uint8_t *const row = tb_bitmap_row(bitmap, (uint32_t)y);
        for (size_t x = 0; x < side; x++)
        {
            const uint8_t noise = (uint8_t)(tb_bench_random(&random) & 7u);
            uint8_t *const pixel = row + x * 4u;
            pixel[0] = (uint8_t)(x * 255u / side + noise);
            pixel[1] = (uint8_t)(y * 255u / side + noise);
            pixel[2] = (uint8_t)(((x / 64u + y / 64u) % 2u) * 160u + noise);
            pixel[3] = (uint8_t)((x + y) % 512u < 256u ? 255u : 128u);
        }
    }
}

void tb_bench_png(tb_bench *bench)
{
    png_bench_state state = {0};
    tb_stream png = {0};
    tb_stream bmp = {0};
    tb_bench_case cases[4] = {0};

    if (!tb_bench_wants(bench, "png"))
    {
        return;
    }
    const size_t side = tb_bench_size(bench, PNG_BENCH_SIDE);
    state.bitmap = tb_bitmap_create((uint32_t)side, (uint32_t)side, TB_BITMAP_RGBA8, NULL);
    png_bench_paint(state.bitmap, side);
    tb_stream_open_buffer(NULL, &png);
    tb_stream_open_buffer(NULL, &bmp);
    (void)tb_bitmap_write_png(state.bitmap, NULL, &png, NULL);
    (void)tb_bitmap_write_bmp(state.bitmap, TB_BITMAP_BMP_32, &bmp, NULL);
    state.png = tb_stream_buffer_bytes(&png, &state.png_length);
    state.bmp = tb_stream_buffer_bytes(&bmp, &state.bmp_length);
    png_bench_join_idat(&state);
    state.rows_length = side * (side * 4u + 1u);
    state.rows = tb_malloc(state.rows_length);

    const double pixels = (double)(side * side);
    const char *const names[4] = {"2048 square RGBA8: decode PNG",
                                  "2048 square RGBA8: inflate its image data",
                                  "2048 square RGBA8: decode BMP", "2048 square RGBA8: encode PNG"};
    tb_bench_function *const runs[4] = {run_decode_png, run_inflate, run_decode_bmp, run_encode};
    for (size_t index = 0; index < 4u; index++)
    {
        cases[index] = (tb_bench_case){.name = names[index],
                                       .variant = "badger",
                                       .run = runs[index],
                                       .context = &state,
                                       .operations = pixels};
    }
    tb_bench_measure(bench, "png", cases, 4);
    tb_free(state.rows);
    tb_free(state.idat);
    (void)tb_stream_close(&png);
    (void)tb_stream_close(&bmp);
    tb_bitmap_destroy(state.bitmap);
}
