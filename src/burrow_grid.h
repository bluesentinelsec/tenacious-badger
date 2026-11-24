/* Burrow's world grid: the texture every graybox surface is drawn with, as
 * Unreal's WorldGridMaterial is. Squares of a meter in two shades of gray,
 * a dark line at every meter and fainter lines at every quarter, so that
 * any surface shows its size and distance at a glance.
 *
 * The texture spans BURROW_GRID_METERS and repeats seamlessly: its lines at
 * the edges are split between the two, half a line on each side.
 *
 * Threading: thread-safe; it touches only the caller's pixels. */
#ifndef BURROW_GRID_H
#define BURROW_GRID_H

#include <stddef.h>
#include <stdint.h>

/* Texels a side of the texture. */
#define BURROW_GRID_SIZE 512u
/* Meters a side of the texture spans: two squares of a meter. */
#define BURROW_GRID_METERS 2.0f
/* Texels from one meter's line to the next. */
#define BURROW_GRID_METER (BURROW_GRID_SIZE / 2u)
/* Texels from one quarter meter's line to the next. */
#define BURROW_GRID_QUARTER (BURROW_GRID_METER / 4u)
/* Bytes of the texture's RGBA pixels. */
#define BURROW_GRID_BYTES ((size_t)BURROW_GRID_SIZE * BURROW_GRID_SIZE * 4u)

/* Fill RGBA pixels, 8 bits a channel in sRGB, rows from the top, with the
 * grid: BURROW_GRID_SIZE a side. `size` is at least BURROW_GRID_BYTES. */
void burrow_grid_fill(uint8_t *pixels, size_t size);

#endif
