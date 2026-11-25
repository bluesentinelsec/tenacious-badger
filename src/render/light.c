/* The raster lighting mode's lights and passes: the scene's lights, kept
 * by handle; the lighting pass, which lights the G-buffer with them and
 * with the sky into the scene's color; and the tonemap pass, which maps
 * that color into the swap chain's. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/sky.h"
#include "badger/sort.h"

#include <math.h>

/* A light's handle: its slot plus 1 in the low bits, its generation above. */
#define TB_LIGHT_SLOT_BITS 16u
#define TB_LIGHT_SLOT_MASK 0xFFFFu
#define TB_LIGHT_GENERATION_MASK 0xFFFFu
/* Bytes of a light as the lighting shader reads it. */
#define TB_PACKED_LIGHT_BYTES 64u
/* The ambient light of a frame without a sky, unless set. */
#define TB_AMBIENT_SKY 0.25f
#define TB_AMBIENT_GROUND 0.08f
/* The lighting pass's flags. */
#define TB_LIGHTING_SPECULAR_AMBIENT 1u

TB_STATIC_ASSERT(sizeof(tb_packed_light) == TB_PACKED_LIGHT_BYTES, "a light in 64 bytes");
TB_STATIC_ASSERT(TB_RENDERER_MAX_LIGHTS <= TB_LIGHT_SLOT_MASK, "a light's slot in its handle");

/* The lighting pass's constants, as the lighting shader's
 * LightingConstants lays them out. */
typedef struct tb_lighting_constants
{
    tb_mat4 clip_to_world; /* the inverse of the frame's clip from the world */
    tb_vec4 eye;           /* the camera's position; w 1 for perspective, 0 orthographic */
    tb_vec4 forward;       /* the way the camera looks */
    tb_vec4 clear;         /* where nothing was drawn */
    tb_vec4 zenith;        /* the ambient light from straight up */
    tb_vec4 horizon;       /* from the horizon */
    tb_vec4 ground;        /* from below */
    tb_vec4 size;          /* the G-buffer's pixels, and one over each */
    uint32_t lights[4];    /* the uploads' bindless index, the lights' offset, count, flags */
    uint32_t sky[4];       /* 1 for a cube map, its index, its sampler's, its levels */
} tb_lighting_constants;

TB_STATIC_ASSERT(sizeof(tb_lighting_constants) <= 256u, "the lighting's constants in 256 bytes");

/* What the tonemap shader reads, as its ToneConstants lays it out. */
typedef struct tb_tonemap_constants
{
    uint32_t scene;   /* the bindless index of the scene's color */
    float exposure;   /* what the scene's color is multiplied by first */
    uint32_t sampler; /* the bindless index of a bilinear, clamped sampler */
    uint32_t unused;  /* to the next 16 bytes */
    float inverse[2]; /* one over the scene's rectangle's width and height */
    float origin[2];  /* its top left in the output's pixels */
} tb_tonemap_constants;

/* A light of the scene, as the frame chooses which to shade. */
typedef struct tb_light_choice
{
    float order; /* -1 for a directional light, else its distance squared */
    uint32_t slot;
} tb_light_choice;

/* ---------------------------------------------------------------------------
 * Lights.
 * ------------------------------------------------------------------------- */

static tb_vec4 tb_light_vec4(tb_vec3 v, float w)
{
    return (tb_vec4){v.x, v.y, v.z, w};
}

/* The way a light goes, of unit length; straight down for none. */
static tb_vec3 tb_light_direction(tb_vec3 direction)
{
    const float length = tb_vec3_length(direction);
    return length > 0.0f ? tb_vec3_scale(direction, 1.0f / length) : (tb_vec3){0.0f, -1.0f, 0.0f};
}

tb_packed_light tb_light_pack(const tb_light *light)
{
    const tb_vec3 radiance = tb_vec3_scale(light->color, light->intensity * TB_PI);
    tb_packed_light packed = {0};

    packed.direction =
        tb_light_vec4(tb_light_direction(light->direction), (float)(uint32_t)light->kind);
    packed.radiance = tb_light_vec4(radiance, 0.0f);
    if (light->kind == TB_LIGHT_DIRECTIONAL)
    {
        return packed;
    }
    packed.position = tb_light_vec4(light->position, 1.0f / (light->range * light->range));
    if (light->kind == TB_LIGHT_SPOT)
    {
        const float outer = cosf(light->outer_angle);
        const float inner = cosf(light->inner_angle);
        packed.cone = (tb_vec4){outer, 1.0f / fmaxf(inner - outer, 1e-4f), 0.0f, 0.0f};
    }
    return packed;
}

static void tb_light_check(const tb_light *light)
{
    TB_ASSERT(light != NULL);
    TB_ASSERT_MSG(light->kind <= TB_LIGHT_SPOT, "a light of a kind there is");
    TB_ASSERT_MSG(light->intensity >= 0.0f, "an intensity of 0 or more");
    TB_ASSERT_MSG(light->kind == TB_LIGHT_POINT || tb_vec3_length(light->direction) > 0.0f,
                  "a direction that is not zero");
    TB_ASSERT_MSG(light->kind == TB_LIGHT_DIRECTIONAL || light->range > 0.0f,
                  "a range of more than 0");
    TB_ASSERT_MSG(light->kind != TB_LIGHT_SPOT ||
                      (light->inner_angle >= 0.0f && light->inner_angle < light->outer_angle &&
                       light->outer_angle < TB_PI * 0.5f),
                  "a cone of angles from 0 to less than a quarter turn, inner below outer");
}

static uint32_t tb_light_handle(size_t slot, uint32_t generation)
{
    return generation << TB_LIGHT_SLOT_BITS | (uint32_t)(slot + 1u);
}

/* The slot a handle names, or SIZE_MAX when it names no light. */
static size_t tb_light_find(const tb_renderer *renderer, tb_light_id id)
{
    const size_t slot = (size_t)(id.id & TB_LIGHT_SLOT_MASK);

    if (slot == 0 || slot > renderer->light_slots)
    {
        return SIZE_MAX;
    }
    if (!renderer->lights[slot - 1].used ||
        renderer->lights[slot - 1].generation != id.id >> TB_LIGHT_SLOT_BITS)
    {
        return SIZE_MAX;
    }
    return slot - 1;
}

tb_light_id tb_renderer_add_light(tb_renderer *renderer, const tb_light *light)
{
    size_t slot = 0;
    tb_light_slot *made = NULL;

    TB_ASSERT(renderer != NULL);
    tb_light_check(light);
    if (renderer->light_count == TB_RENDERER_MAX_LIGHTS)
    {
        return (tb_light_id){0};
    }
    while (slot < renderer->light_slots && renderer->lights[slot].used)
    {
        slot++;
    }
    if (slot == renderer->light_slots)
    {
        if (renderer->light_slots == renderer->light_capacity)
        {
            renderer->light_capacity =
                renderer->light_capacity == 0 ? 16 : renderer->light_capacity * 2;
            renderer->lights = tb_realloc_array(renderer->lights, renderer->light_capacity,
                                                sizeof(*renderer->lights));
        }
        renderer->lights[slot] = (tb_light_slot){0};
        renderer->light_slots++;
    }
    made = &renderer->lights[slot];
    /* A slot's generation moves on each time it is taken, skipping 0, so a
     * handle of a light removed from it names nothing. */
    made->generation = (made->generation + 1u) & TB_LIGHT_GENERATION_MASK;
    if (made->generation == 0)
    {
        made->generation = 1;
    }
    made->light = *light;
    made->used = true;
    renderer->light_count++;
    return (tb_light_id){tb_light_handle(slot, made->generation)};
}

bool tb_renderer_set_light(tb_renderer *renderer, tb_light_id id, const tb_light *light)
{
    size_t slot = 0;

    TB_ASSERT(renderer != NULL);
    tb_light_check(light);
    slot = tb_light_find(renderer, id);
    if (slot == SIZE_MAX)
    {
        return false;
    }
    renderer->lights[slot].light = *light;
    return true;
}

bool tb_renderer_get_light(const tb_renderer *renderer, tb_light_id id, tb_light *light)
{
    size_t slot = 0;

    TB_ASSERT(renderer != NULL && light != NULL);
    slot = tb_light_find(renderer, id);
    if (slot == SIZE_MAX)
    {
        return false;
    }
    *light = renderer->lights[slot].light;
    return true;
}

void tb_renderer_remove_light(tb_renderer *renderer, tb_light_id id)
{
    size_t slot = 0;

    TB_ASSERT(renderer != NULL);
    slot = tb_light_find(renderer, id);
    if (slot == SIZE_MAX)
    {
        return;
    }
    renderer->lights[slot].used = false;
    renderer->light_count--;
}

size_t tb_renderer_light_count(const tb_renderer *renderer)
{
    TB_ASSERT(renderer != NULL);
    return renderer->light_count;
}

void tb_renderer_set_ambient(tb_renderer *renderer, tb_vec3 sky, tb_vec3 ground)
{
    TB_ASSERT(renderer != NULL);
    renderer->ambient_sky = sky;
    renderer->ambient_ground = ground;
}

tb_light tb_renderer_default_light(const tb_renderer *renderer)
{
    const tb_sky *const sky = &renderer->sky;
    tb_light light = TB_LIGHT_INIT;
    tb_vec3 forward = {0};

    light.kind = TB_LIGHT_DIRECTIONAL;
    if (sky->kind == TB_SKY_PROCEDURAL && tb_vec3_length(sky->sun_direction) > 0.0f &&
        sky->sun_radius > 0.0f)
    {
        light.direction = tb_vec3_normalize(tb_vec3_negate(sky->sun_direction));
        light.color = sky->sun_color;
        return light;
    }
    /* From above and a little behind the camera, as meshes were lit before
     * the lighting mode. */
    forward = tb_quat_rotate(renderer->camera.orientation, (tb_vec3){0, 0, -1});
    light.direction =
        tb_vec3_normalize(tb_vec3_add((tb_vec3){0.3f, -1.0f, 0.2f}, tb_vec3_scale(forward, 0.5f)));
    return light;
}

static int tb_light_choice_compare(const void *left, const void *right, void *context)
{
    const tb_light_choice *const a = left;
    const tb_light_choice *const b = right;

    (void)context;
    if (a->order != b->order)
    {
        return a->order < b->order ? -1 : 1;
    }
    return a->slot < b->slot ? -1 : (a->slot > b->slot ? 1 : 0);
}

/* Write the lights the frame shades when it has more than the quality
 * allows: the directional first, then the nearest to the camera. */
static uint64_t tb_lights_upload_nearest(tb_renderer *renderer, uint32_t *count)
{
    tb_light_choice *const choices = tb_calloc(renderer->light_count, sizeof(*choices));
    size_t chosen = 0;
    uint64_t offset = UINT64_MAX;

    for (size_t slot = 0; slot < renderer->light_slots; slot++)
    {
        const tb_light *const light = &renderer->lights[slot].light;
        if (!renderer->lights[slot].used)
        {
            continue;
        }
        choices[chosen].slot = (uint32_t)slot;
        choices[chosen].order =
            light->kind == TB_LIGHT_DIRECTIONAL
                ? -1.0f
                : tb_vec3_length_squared(tb_vec3_sub(light->position, renderer->camera.position));
        chosen++;
    }
    tb_sort(choices, chosen, sizeof(*choices), tb_light_choice_compare, NULL);
    for (size_t index = 0; index < renderer->quality.max_lights; index++)
    {
        const tb_packed_light packed = tb_light_pack(&renderer->lights[choices[index].slot].light);
        const uint64_t at =
            tb_renderer_upload(renderer, &packed, sizeof(packed), TB_PACKED_LIGHT_BYTES);
        offset = index == 0 ? at : offset;
    }
    *count = renderer->quality.max_lights;
    tb_free(choices);
    return offset;
}

/* Write the frame's lights to its uploads, or its default light when it
 * has none: their offset, or UINT64_MAX when there is no room. */
static uint64_t tb_lights_upload(tb_renderer *renderer, uint32_t *count)
{
    /* One after another, so that the shader finds the lights at one offset. */
    const uint64_t needed = TB_PACKED_LIGHT_BYTES * (uint64_t)(renderer->light_count + 1u);
    uint64_t offset = UINT64_MAX;

    *count = 0;
    if (tb_renderer_upload_room(renderer, TB_PACKED_LIGHT_BYTES) < needed)
    {
        return UINT64_MAX;
    }
    if (renderer->light_count == 0)
    {
        const tb_light light = tb_renderer_default_light(renderer);
        const tb_packed_light packed = tb_light_pack(&light);
        *count = 1;
        return tb_renderer_upload(renderer, &packed, sizeof(packed), TB_PACKED_LIGHT_BYTES);
    }
    if (renderer->light_count > renderer->quality.max_lights)
    {
        return tb_lights_upload_nearest(renderer, count);
    }
    for (size_t slot = 0; slot < renderer->light_slots; slot++)
    {
        tb_packed_light packed = {0};
        uint64_t at = 0;
        if (!renderer->lights[slot].used)
        {
            continue;
        }
        packed = tb_light_pack(&renderer->lights[slot].light);
        at = tb_renderer_upload(renderer, &packed, sizeof(packed), TB_PACKED_LIGHT_BYTES);
        offset = *count == 0 ? at : offset;
        (*count)++;
    }
    return offset;
}

/* The sky's part of the lighting's constants: the ambient light's colors,
 * and its cube map. */
static void tb_lighting_sky(const tb_renderer *renderer, tb_lighting_constants *constants)
{
    const tb_sky *const sky = &renderer->sky;

    constants->zenith = tb_light_vec4(renderer->ambient_sky, 1.0f);
    constants->horizon = tb_light_vec4(renderer->ambient_sky, 1.0f);
    constants->ground = tb_light_vec4(renderer->ambient_ground, 1.0f);
    if (sky->kind == TB_SKY_PROCEDURAL)
    {
        constants->zenith = tb_light_vec4(sky->zenith, 1.0f);
        constants->horizon = tb_light_vec4(sky->horizon, 1.0f);
        constants->ground = tb_light_vec4(sky->ground, 1.0f);
    }
    if (sky->kind == TB_SKY_CUBE)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mips = 0;
        tb_texture_size(sky->cube, &width, &height, &mips);
        constants->sky[0] = 1;
        constants->sky[1] = tb_texture_bindless_index(sky->cube);
        constants->sky[2] = tb_rhi_sampler_index(renderer->device, renderer->material_sampler);
        constants->sky[3] = mips;
    }
}

uint64_t tb_renderer_lighting_upload(tb_renderer *renderer, tb_mat4 clip)
{
    const tb_camera *const camera = &renderer->camera;
    tb_lighting_constants constants = {0};
    uint32_t count = 0;
    const uint64_t lights = tb_lights_upload(renderer, &count);

    if (lights == UINT64_MAX || !tb_mat4_inverse(clip, &constants.clip_to_world))
    {
        return UINT64_MAX;
    }
    constants.eye =
        tb_light_vec4(camera->position, camera->projection == TB_CAMERA_PERSPECTIVE ? 1.0f : 0.0f);
    constants.forward =
        tb_light_vec4(tb_quat_rotate(camera->orientation, (tb_vec3){0, 0, -1}), 0.0f);
    constants.clear = renderer->clear;
    constants.size =
        (tb_vec4){(float)renderer->render_width, (float)renderer->render_height,
                  1.0f / (float)renderer->render_width, 1.0f / (float)renderer->render_height};
    constants.lights[0] =
        tb_rhi_buffer_index(renderer->device, renderer->frames[renderer->slot].upload);
    constants.lights[1] = (uint32_t)lights;
    constants.lights[2] = count;
    constants.lights[3] = renderer->quality.reflections ? TB_LIGHTING_SPECULAR_AMBIENT : 0u;
    tb_lighting_sky(renderer, &constants);
    return tb_renderer_upload(renderer, &constants, sizeof(constants), TB_RHI_CONSTANT_ALIGNMENT);
}

void tb_renderer_lighting_record(tb_renderer *renderer, tb_rhi_command_list *list,
                                 uint64_t constants, const tb_renderer_gbuffer_indices *gbuffer)
{
    if (constants == UINT64_MAX)
    {
        return;
    }
    tb_rhi_cmd_pipeline(list, renderer->lighting_pipeline);
    tb_rhi_cmd_push_constants(list, gbuffer, sizeof(*gbuffer));
    tb_rhi_cmd_constant_buffer(list, 0, renderer->frames[renderer->slot].upload, constants);
    tb_rhi_cmd_draw(list, 3, 1, 0, 0);
}

void tb_renderer_tonemap_record(tb_renderer *renderer, tb_rhi_command_list *list, uint32_t scene)
{
    const tb_tonemap_constants constants = {
        .scene = scene,
        .exposure = 1.0f,
        .sampler = tb_rhi_sampler_index(renderer->device, renderer->canvas_samplers[0]),
        .inverse = {1.0f / (float)renderer->scene_rect.width,
                    1.0f / (float)renderer->scene_rect.height},
        .origin = {(float)renderer->scene_rect.x, (float)renderer->scene_rect.y}};

    tb_renderer_cmd_viewport(renderer, list);
    tb_rhi_cmd_pipeline(list, renderer->tonemap_pipeline);
    tb_rhi_cmd_push_constants(list, &constants, sizeof(constants));
    tb_rhi_cmd_draw(list, 3, 1, 0, 0);
}

/* ---------------------------------------------------------------------------
 * The renderer's part.
 * ------------------------------------------------------------------------- */

tb_rhi_result tb_renderer_lighting_create(tb_renderer *renderer, tb_rhi_error *error)
{
    tb_rhi_graphics_pipeline_desc desc = {0};
    tb_rhi_result result = TB_RHI_OK;

    renderer->ambient_sky = (tb_vec3){TB_AMBIENT_SKY, TB_AMBIENT_SKY, TB_AMBIENT_SKY};
    renderer->ambient_ground = (tb_vec3){TB_AMBIENT_GROUND, TB_AMBIENT_GROUND, TB_AMBIENT_GROUND};
    if (!renderer->deferred)
    {
        return TB_RHI_OK;
    }
    desc.colors[0] = TB_RENDERER_SCENE_FORMAT;
    desc.color_count = 1;
    desc.name = "lighting";
    result = tb_renderer_pipeline(renderer, "/Engine/Private/DeferredLighting.hlsl", NULL, &desc,
                                  &renderer->lighting_pipeline, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    desc.colors[0] = TB_RENDERER_COLOR_FORMAT;
    desc.name = "tonemap";
    return tb_renderer_pipeline(renderer, "/Engine/Private/Tonemap.hlsl", NULL, &desc,
                                &renderer->tonemap_pipeline, error);
}

void tb_renderer_lighting_destroy(tb_renderer *renderer)
{
    tb_rhi_pipeline_destroy(renderer->device, renderer->lighting_pipeline);
    tb_rhi_pipeline_destroy(renderer->device, renderer->tonemap_pipeline);
}
