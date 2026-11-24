/* Pixels for images, made in memory: checkerboards, gradients and noise
 * for tools and samples before assets are loaded, and premultiplying. */
#include "canvas_internal.h"

#include "badger/assert.h"
#include "badger/hash.h"

#include <math.h>

/* Bits of fraction of the noise's fixed-point blending. */
#define TB_NOISE_FRACTION_BITS 16u
#define TB_NOISE_ONE (1u << TB_NOISE_FRACTION_BITS)

static uint8_t tb_image_channel(float value)
{
    const float clamped = fminf(fmaxf(value, 0.0f), 1.0f);
    return (uint8_t)(clamped * 255.0f + 0.5f);
}

static void tb_image_put(uint8_t *pixel, tb_vec4 color)
{
    pixel[0] = tb_image_channel(color.x);
    pixel[1] = tb_image_channel(color.y);
    pixel[2] = tb_image_channel(color.z);
    pixel[3] = tb_image_channel(color.w);
}

static void tb_image_check_size(size_t size, uint32_t width, uint32_t height)
{
    TB_ASSERT_MSG(width > 0 && height > 0 && (size_t)width <= SIZE_MAX / 4u / height &&
                      size >= (size_t)width * height * 4u,
                  "room for width * height RGBA pixels");
}

void tb_image_fill_checkerboard(uint8_t *pixels, size_t size, uint32_t width, uint32_t height,
                                uint32_t cell, tb_vec4 a, tb_vec4 b)
{
    TB_ASSERT(pixels != NULL);
    TB_ASSERT_MSG(cell > 0, "a checkerboard's squares have a size");
    tb_image_check_size(size, width, height);
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            const bool first = ((x / cell + y / cell) % 2u) == 0;
            tb_image_put(&pixels[((size_t)y * width + x) * 4u], first ? a : b);
        }
    }
}

/* How far along a gradient a pixel's middle is, from 0 to 1. */
static float tb_image_gradient_at(tb_image_gradient direction, uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height)
{
    const float u = ((float)x + 0.5f) / (float)width;
    const float v = ((float)y + 0.5f) / (float)height;

    switch (direction)
    {
    case TB_IMAGE_GRADIENT_HORIZONTAL:
        return u;
    case TB_IMAGE_GRADIENT_VERTICAL:
        return v;
    case TB_IMAGE_GRADIENT_RADIAL:
        break;
    }
    /* Distance from the middle, 1 at the middle of the nearest edge. */
    return fminf(2.0f * sqrtf((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)), 1.0f);
}

void tb_image_fill_gradient(uint8_t *pixels, size_t size, uint32_t width, uint32_t height,
                            tb_image_gradient direction, tb_vec4 from, tb_vec4 to)
{
    TB_ASSERT(pixels != NULL);
    tb_image_check_size(size, width, height);
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            const float t = tb_image_gradient_at(direction, x, y, width, height);
            const tb_vec4 color = {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                                   from.z + (to.z - from.z) * t, from.w + (to.w - from.w) * t};
            tb_image_put(&pixels[((size_t)y * width + x) * 4u], color);
        }
    }
}

/* The noise's value, 0 to 255, at a point of its lattice. */
static uint32_t tb_noise_lattice(uint64_t seed, uint32_t x, uint32_t y)
{
    return (uint32_t)(tb_hash_u64(seed ^ tb_hash_u64((uint64_t)x << 32 | y)) >> 56);
}

/* Smoothstep of a fraction in fixed point: 3t² - 2t³, in integers so that
 * every platform makes the same pixels. */
static uint64_t tb_noise_smooth(uint64_t t)
{
    const uint64_t squared = t * t >> TB_NOISE_FRACTION_BITS;
    const uint64_t cubed = squared * t >> TB_NOISE_FRACTION_BITS;
    return 3u * squared - 2u * cubed;
}

static uint64_t tb_noise_blend(uint64_t a, uint64_t b, uint64_t t)
{
    return (a * (TB_NOISE_ONE - t) + b * t) >> TB_NOISE_FRACTION_BITS;
}

void tb_image_fill_noise(uint8_t *pixels, size_t size, uint32_t width, uint32_t height,
                         uint32_t cell, uint64_t seed)
{
    TB_ASSERT(pixels != NULL);
    TB_ASSERT_MSG(cell > 0, "noise's cells have a size");
    tb_image_check_size(size, width, height);
    for (uint32_t y = 0; y < height; y++)
    {
        const uint32_t row = y / cell;
        const uint64_t ty =
            tb_noise_smooth(((uint64_t)(y % cell) << TB_NOISE_FRACTION_BITS) / cell);
        for (uint32_t x = 0; x < width; x++)
        {
            const uint32_t column = x / cell;
            const uint64_t tx =
                tb_noise_smooth(((uint64_t)(x % cell) << TB_NOISE_FRACTION_BITS) / cell);
            const uint64_t top = tb_noise_blend(tb_noise_lattice(seed, column, row),
                                                tb_noise_lattice(seed, column + 1u, row), tx);
            const uint64_t bottom =
                tb_noise_blend(tb_noise_lattice(seed, column, row + 1u),
                               tb_noise_lattice(seed, column + 1u, row + 1u), tx);
            const uint8_t value = (uint8_t)tb_noise_blend(top, bottom, ty);
            uint8_t *const pixel = &pixels[((size_t)y * width + x) * 4u];
            pixel[0] = value;
            pixel[1] = value;
            pixel[2] = value;
            pixel[3] = UINT8_MAX;
        }
    }
}

void tb_image_premultiply(uint8_t *pixels, size_t count)
{
    TB_ASSERT(pixels != NULL || count == 0);
    for (size_t index = 0; index < count; index++)
    {
        uint8_t *const pixel = &pixels[index * 4u];
        const uint32_t alpha = pixel[3];
        for (size_t channel = 0; channel < 3; channel++)
        {
            pixel[channel] = (uint8_t)((pixel[channel] * alpha + 127u) / 255u);
        }
    }
}
