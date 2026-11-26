/* Cameras: where a view is, which way it looks, and how it projects, in the
 * engine's conventions (right-handed, Y up, looking down -Z, reversed
 * depth). */
#include "badger/renderer.h"

#include "badger/assert.h"

#include <math.h>
#include <stddef.h>

/* How close to straight up or down a camera may look before "up" is lost. */
#define TB_CAMERA_MOST_VERTICAL 0.9999f
/* How near the view's aspect is to the reference's to be the same shape,
 * computed two ways and rounded. */
#define TB_CAMERA_SAME_ASPECT 1e-5f

static const char *const tb_camera_axis_constraint_names[TB_CAMERA_AXIS_COUNT] = {
    "design", "vertical", "horizontal"};

tb_camera tb_camera_perspective(float vertical_fov)
{
    TB_ASSERT_MSG(vertical_fov > 0.0f && vertical_fov < TB_PI, "a field of view out of range");
    return (tb_camera){.orientation = tb_quat_identity(),
                       .projection = TB_CAMERA_PERSPECTIVE,
                       .vertical_fov = vertical_fov,
                       .near_plane = 0.1f};
}

tb_camera tb_camera_orthographic(float height)
{
    TB_ASSERT_MSG(height > 0.0f, "an orthographic view of no height");
    return (tb_camera){.orientation = tb_quat_identity(),
                       .projection = TB_CAMERA_ORTHOGRAPHIC,
                       .height = height,
                       .near_plane = -1000.0f,
                       .far_plane = 1000.0f};
}

bool tb_camera_look_at(tb_camera *camera, tb_vec3 eye, tb_vec3 target)
{
    const tb_vec3 forward = tb_vec3_sub(target, eye);
    const float length = tb_vec3_length(forward);
    tb_mat4 view = tb_mat4_identity();

    TB_ASSERT(camera != NULL);
    if (length <= 0.0f || fabsf(forward.y / length) > TB_CAMERA_MOST_VERTICAL ||
        !tb_mat4_look_at(eye, target, (tb_vec3){0.0f, 1.0f, 0.0f}, &view))
    {
        return false;
    }
    /* The view turns the world into the camera's space; the camera's own
     * rotation is the inverse, the transpose of the view's rotation. */
    camera->orientation =
        tb_quat_normalize(tb_quat_from_mat3(tb_mat3_transpose(tb_mat3_from_mat4(view))));
    camera->position = eye;
    return true;
}

tb_mat4 tb_camera_view(const tb_camera *camera)
{
    TB_ASSERT(camera != NULL);
    return tb_mat4_mul(tb_mat4_rotation(tb_quat_conjugate(camera->orientation)),
                       tb_mat4_translation(tb_vec3_negate(camera->position)));
}

/* How much more than the camera's own vertical extent a view of an aspect
 * shows down, its field of view given at a reference aspect: 1 where the
 * vertical extent holds, more where the reference's horizontal extent
 * holds in a taller view, less where it holds in a wider one. */
static float tb_camera_vertical_stretch(const tb_camera *camera, float aspect,
                                        float reference_aspect)
{
    if (!(reference_aspect > 0.0f) || !isfinite(reference_aspect))
    {
        return 1.0f;
    }
    /* Within float rounding of the reference, the view is the reference's,
     * and its vertical extent holds exactly. */
    const float across = reference_aspect / aspect;
    if (fabsf(across - 1.0f) <= TB_CAMERA_SAME_ASPECT)
    {
        return 1.0f;
    }
    switch (camera->axis_constraint)
    {
    case TB_CAMERA_AXIS_HORIZONTAL:
        return across;
    case TB_CAMERA_AXIS_DESIGN:
        return across > 1.0f ? across : 1.0f;
    case TB_CAMERA_AXIS_VERTICAL:
    case TB_CAMERA_AXIS_COUNT:
        break;
    }
    return 1.0f;
}

tb_mat4 tb_camera_projection_of(const tb_camera *camera, float aspect)
{
    return tb_camera_projection_fit(camera, aspect, 0.0f);
}

tb_mat4 tb_camera_projection_fit(const tb_camera *camera, float aspect, float reference_aspect)
{
    TB_ASSERT(camera != NULL);
    TB_ASSERT_MSG(aspect > 0.0f, "an aspect of no width");
    const float stretch = tb_camera_vertical_stretch(camera, aspect, reference_aspect);
    const float far_plane = camera->far_plane > 0.0f ? camera->far_plane : INFINITY;
    if (camera->projection == TB_CAMERA_ORTHOGRAPHIC)
    {
        const float half = camera->height * 0.5f * stretch;
        return tb_mat4_orthographic(-half * aspect, half * aspect, -half, half, camera->near_plane,
                                    camera->far_plane);
    }
    if (stretch == 1.0f)
    {
        return tb_mat4_perspective(camera->vertical_fov, aspect, camera->near_plane, far_plane);
    }
    const float fov = 2.0f * atanf(tanf(camera->vertical_fov * 0.5f) * stretch);
    return tb_mat4_perspective(fov, aspect, camera->near_plane, far_plane);
}

tb_vec2 tb_camera_half_extent(const tb_camera *camera, float aspect, float reference_aspect)
{
    TB_ASSERT(camera != NULL);
    TB_ASSERT_MSG(aspect > 0.0f, "an aspect of no width");
    const float stretch = tb_camera_vertical_stretch(camera, aspect, reference_aspect);
    const float half = camera->projection == TB_CAMERA_ORTHOGRAPHIC
                           ? camera->height * 0.5f * stretch
                           : tanf(camera->vertical_fov * 0.5f) * stretch;
    return (tb_vec2){half * aspect, half};
}

const char *tb_camera_axis_constraint_name(tb_camera_axis_constraint constraint)
{
    return (size_t)constraint < TB_CAMERA_AXIS_COUNT ? tb_camera_axis_constraint_names[constraint]
                                                     : "unknown";
}
