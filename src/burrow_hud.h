/* Burrow's HUD: the 2D overlay over the room, drawn through the canvas in
 * the UI role's font of the engine's font catalog. It shows the frame rate
 * as `interface.performance_overlay` asks, the controls, and a crosshair as
 * `interface.crosshair` asks, at the opacity of `interface.hud_opacity`, in
 * logical units, so that it keeps its size on every display.
 *
 * Threading: stage-owned; called on the thread that renders. */
#ifndef BURROW_HUD_H
#define BURROW_HUD_H

#include "badger/engine_settings.h"
#include "badger/renderer.h"

#include <stdbool.h>

/* Seconds between changes of the frame rate shown, so that it can be read. */
#define BURROW_RATE_SHOWN_EVERY 0.25

/* The frame rate, averaged so that it reads steadily. */
typedef struct burrow_frame_rate
{
    double average; /* Seconds a frame, a moving average; 0 before the first. */
    double shown;   /* The average when it was last shown. */
    double since;   /* Seconds since it was last shown. */
    bool has_shown; /* An average has been shown. */
} burrow_frame_rate;

/* Count a frame of `seconds`. Returns true when the shown rate changed. */
bool burrow_frame_rate_add(burrow_frame_rate *rate, double seconds);

/* Frames a second, as shown; 0 before any. */
double burrow_frame_rate_per_second(const burrow_frame_rate *rate);

/* What the HUD draws with. Made by burrow_hud_create(); released by
 * burrow_hud_destroy() before the renderer. */
typedef struct burrow_hud burrow_hud;

/* Open the HUD's font. A build of the engine that carries no fonts draws
 * the crosshair alone. Never NULL. */
burrow_hud *burrow_hud_create(void);

/* Release the HUD, outside a frame. NULL is ignored. */
void burrow_hud_destroy(burrow_hud *hud, tb_renderer *renderer);

/* Count a frame of `seconds` toward the frame rate shown. */
void burrow_hud_count_frame(burrow_hud *hud, double seconds);

/* Draw the HUD this frame, over the scene, between the renderer's begin and
 * end. `captured` says whether the mouse is the player's, which changes the
 * controls it names. */
void burrow_hud_draw(burrow_hud *hud, tb_renderer *renderer, const tb_engine_settings *engine,
                     bool captured);

#endif
