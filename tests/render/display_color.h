/* What the renderer's output transform makes of a linear color, for tests
 * that read the pixels of a frame: Narkowicz's fit of the ACES filmic curve
 * to each channel, its input scaled by 0.6, then sRGB's encoding, rounded
 * to 8 bits, as the tonemap pass writes the swap chain. Written apart from the renderer's shader,
 * so that a change to either shows. */
#ifndef TB_TEST_DISPLAY_COLOR_H
#define TB_TEST_DISPLAY_COLOR_H

#include <math.h>
#include <stdint.h>

/* One channel of linear light as the frame shows it, 0 to 255. */
static inline int display_channel(float linear)
{
    const float x = (linear > 0.0f ? linear : 0.0f) * 0.6f;
    float mapped = x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f);
    float encoded = 0.0f;

    mapped = mapped < 1.0f ? mapped : 1.0f;
    encoded = mapped <= 0.0031308f ? mapped * 12.92f : 1.055f * powf(mapped, 1.0f / 2.4f) - 0.055f;
    return (int)lroundf(encoded * 255.0f);
}

/* A linear color as the frame shows it, as 0xRRGGBB. */
static inline uint32_t display_rgb(float red, float green, float blue)
{
    return (uint32_t)display_channel(red) << 16 | (uint32_t)display_channel(green) << 8 |
           (uint32_t)display_channel(blue);
}

#endif
