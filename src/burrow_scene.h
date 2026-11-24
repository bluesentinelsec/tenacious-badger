/* Burrow's scene: a room made into what the renderer draws, through its
 * public interface alone: a mesh for each piece, the world grid's texture,
 * a material for each surface, and the sky. Whatever lighting the renderer
 * draws its meshes and materials with, the room is drawn with.
 *
 * Threading: stage-owned; called on the thread that renders. */
#ifndef BURROW_SCENE_H
#define BURROW_SCENE_H

#include "burrow_room.h"

#include "badger/material.h"
#include "badger/renderer.h"

/* A room's meshes and materials. Made by burrow_scene_create(); released
 * by burrow_scene_destroy() before the renderer. */
typedef struct burrow_scene burrow_scene;

/* Make what draws a room, and set the renderer's sky. The room outlives the
 * scene. Never NULL; running out of memory is fatal. */
burrow_scene *burrow_scene_create(tb_renderer *renderer, const burrow_room *room);

/* Release a scene, outside a frame. NULL is ignored. */
void burrow_scene_destroy(burrow_scene *scene, tb_renderer *renderer);

/* Draw the room this frame, between the renderer's begin and end. */
void burrow_scene_draw(const burrow_scene *scene, tb_renderer *renderer);

#endif
