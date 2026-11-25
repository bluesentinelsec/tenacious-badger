/* Golden images, for the tests that compare what the renderer drew on WARP
 * with a frame looked at by eye (tests/data/golden, docs/testing.md):
 * Windows only, as WARP is. */
#ifndef TB_TEST_GOLDEN_H
#define TB_TEST_GOLDEN_H

#include "badger/window.h"

#include <stdint.h>

/* The frame of the golden images, in pixels: a size that whole logical
 * sizes reach at display scales of 100%, 125%, 150%, 200% and 250%. */
#define GOLDEN_WIDTH 240u
#define GOLDEN_HEIGHT 180u

/* A window of the golden images' size in pixels, whatever the display's
 * scale, or a skipped case at a scale that cannot make it, the system
 * closed. */
tb_window *golden_window(tb_window_system *system, const char *title);

/* Compare a frame, 4 bytes a pixel in BGRA, with its golden image, within
 * the tolerances; on a difference, or without one, fail and write the
 * frame beside the tests as `<name>.actual.bmp`, to look at or to take as
 * the golden image. */
void expect_golden(const char *name, const uint8_t *pixels, uint32_t width, uint32_t height);

#endif
