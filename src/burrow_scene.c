/* Burrow's scene: the room's meshes, the grid's texture and materials, and
 * the sky. */
#include "burrow_scene.h"

#include "burrow_grid.h"

#include "badger/memory.h"
#include "badger/sky.h"

struct burrow_scene
{
    const burrow_room *room;
    tb_mesh **meshes; /* One for each of the room's pieces, in its order. */
    tb_texture *grid;
    tb_material *materials[BURROW_SURFACE_COUNT];
};

/* The grid's tint on each surface, in linear RGB. */
static const tb_vec3 burrow_surface_tints[BURROW_SURFACE_COUNT] = {
    [BURROW_SURFACE_FLOOR] = {0.80f, 0.80f, 0.80f},
    [BURROW_SURFACE_WALL] = {1.00f, 1.00f, 1.00f},
    [BURROW_SURFACE_BLOCK] = {0.75f, 0.82f, 1.00f},
    [BURROW_SURFACE_ACCENT] = {1.00f, 0.55f, 0.20f},
};

static const char *const burrow_surface_names[BURROW_SURFACE_COUNT] = {
    [BURROW_SURFACE_FLOOR] = "burrow floor",
    [BURROW_SURFACE_WALL] = "burrow wall",
    [BURROW_SURFACE_BLOCK] = "burrow block",
    [BURROW_SURFACE_ACCENT] = "burrow accent",
};

static tb_texture *burrow_make_grid(tb_renderer *renderer)
{
    uint8_t *const pixels = tb_malloc(BURROW_GRID_BYTES);
    tb_texture *texture = NULL;

    burrow_grid_fill(pixels, BURROW_GRID_BYTES);
    texture = tb_texture_create(renderer,
                                &(tb_texture_desc){.width = BURROW_GRID_SIZE,
                                                   .height = BURROW_GRID_SIZE,
                                                   .content = TB_TEXTURE_COLOR,
                                                   .name = "burrow world grid"},
                                pixels, BURROW_GRID_BYTES);
    tb_free(pixels);
    return texture;
}

static void burrow_make_materials(burrow_scene *scene, tb_renderer *renderer)
{
    /* The pieces' texture coordinates are meters; the grid spans a few. */
    const float repeat = 1.0f / BURROW_GRID_METERS;

    for (size_t surface = 0; surface < BURROW_SURFACE_COUNT; surface++)
    {
        const tb_vec3 tint = burrow_surface_tints[surface];
        tb_material_desc desc = TB_MATERIAL_DESC_INIT;
        desc.base_color = scene->grid;
        desc.base_color_factor = (tb_vec4){tint.x, tint.y, tint.z, 1.0f};
        desc.roughness = 0.8f;
        desc.uv_scale = (tb_vec2){repeat, repeat};
        desc.name = burrow_surface_names[surface];
        scene->materials[surface] = tb_material_create(renderer, &desc);
    }
}

burrow_scene *burrow_scene_create(tb_renderer *renderer, const burrow_room *room)
{
    burrow_scene *const scene = tb_calloc(1, sizeof(*scene));
    burrow_geometry *const geometry = tb_calloc(1, sizeof(*geometry));
    const tb_sky sky = TB_SKY_PROCEDURAL_INIT;

    scene->room = room;
    scene->meshes = tb_calloc(room->piece_count, sizeof(*scene->meshes));
    for (size_t index = 0; index < room->piece_count; index++)
    {
        burrow_piece_geometry(&room->pieces[index], geometry);
        scene->meshes[index] = tb_mesh_create(renderer, geometry->vertices, geometry->vertex_count,
                                              geometry->indices, geometry->index_count);
    }
    tb_free(geometry);
    scene->grid = burrow_make_grid(renderer);
    burrow_make_materials(scene, renderer);
    tb_renderer_set_sky(renderer, &sky);
    return scene;
}

void burrow_scene_destroy(burrow_scene *scene, tb_renderer *renderer)
{
    if (scene == NULL)
    {
        return;
    }
    tb_renderer_set_sky(renderer, NULL);
    for (size_t surface = 0; surface < BURROW_SURFACE_COUNT; surface++)
    {
        tb_material_destroy(renderer, scene->materials[surface]);
    }
    tb_texture_destroy(renderer, scene->grid);
    for (size_t index = 0; index < scene->room->piece_count; index++)
    {
        tb_mesh_destroy(renderer, scene->meshes[index]);
    }
    tb_free(scene->meshes);
    tb_free(scene);
}

void burrow_scene_draw(const burrow_scene *scene, tb_renderer *renderer)
{
    const tb_mat4 placed = tb_mat4_identity();

    /* The pieces' triangles are in the world's space already. */
    for (size_t index = 0; index < scene->room->piece_count; index++)
    {
        const burrow_surface surface = scene->room->pieces[index].surface;
        tb_renderer_draw_mesh_material(renderer, scene->meshes[index], placed,
                                       scene->materials[surface]);
    }
}
