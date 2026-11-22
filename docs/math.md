# Math

`badger/math.h` has the vectors, quaternions and matrices that transforms,
cameras, animation and physics are made of. `badger/geometry.h` has the shapes
that culling, picking and physics queries test: planes, boxes, spheres, rays
and view frustums.

```c
const tb_mat4 model = tb_mat4_from_trs(position, tb_quat_from_axis_angle(up, yaw), scale);
tb_mat4 view = tb_mat4_identity();
(void)tb_mat4_look_at(eye, target, (tb_vec3){0.0f, 1.0f, 0.0f}, &view);
const tb_mat4 projection = tb_mat4_perspective(tb_radians(70.0f), aspect, 0.1f, INFINITY);
const tb_mat4 view_projection = tb_mat4_mul(projection, view);
const tb_frustum frustum = tb_frustum_from_matrix(view_projection);

if (tb_frustum_test_aabb(&frustum, tb_aabb_transform(mesh_bounds, model)) !=
    TB_CONTAINMENT_OUTSIDE)
{
    draw(mesh, tb_mat4_mul(view_projection, model));
}
```

## Conventions

Every part of the engine uses these, so a game never converts between
modules, and a backend converts once, out of sight.

| What | Convention | Why |
| --- | --- | --- |
| Handedness | Right-handed: +X right, +Y up, a camera looks down −Z | glTF's, so imported models need no conversion; Godot's and OpenGL's |
| Positive angles | Counterclockwise, seen from the axis's positive end | The right-hand rule that goes with right-handed axes |
| Vectors and matrices | Column vectors: a matrix transforms `M * v`, and `tb_mat4_mul(a, b)` applies `b` first | The convention of textbooks, HLSL's `mul(M, v)`, and GLSL |
| Storage | A matrix's 16 floats are its columns in order; the translation is the last column | What shader constant buffers take by default, so a matrix uploads as it is |
| Clip space | x and y from −1 to 1 with y up; depth from 0 to 1 | D3D's, Vulkan's and Metal's. The OpenGL backend sets a 0-to-1 depth range, or converts where it can't, and the Vulkan backend flips y, so games never see a difference |
| Depth | Reversed: the near plane is at 1, the far plane at 0, and a far plane at infinity is allowed | Floats are dense near 0, and reversed depth spends that precision on distance, which ends z-fighting far away |
| Angles | Radians | What sine and cosine take. `tb_radians()` converts the degrees that settings and menus show |
| Quaternions | `x, y, z, w`, `w` the scalar part; `tb_quat_mul(a, b)` rotates by `b`, then `a` | The same order as matrices |

A renderer using reversed depth clears depth to 0 and keeps fragments whose
depth is greater.

`tb_vec3` is three packed floats, 12 bytes. A shader constant buffer pads a
`float3` to 16 bytes, so the structures the engine shares with shaders spell
out their padding.

## What is inline and what is not

Vectors, quaternions and scalar helpers are `static inline` in the header.
They are values passed and returned by value, and each costs what its
arithmetic costs, with no call into the engine library. Everything else
(matrices, rotation blends, conversions and all of `geometry.h`) is a
function of the library.

Those two are the exception to the rule that a module's header declares only
what the library exports: a call into a shared library for each vector
addition would cost more than the addition.

## SIMD

Products of 4×4 matrices and transforms of many vectors or points use SIMD:
SSE2 on x64, which every x64 processor has, and NEON on 64-bit ARM, which
every one of those has, Apple Silicon included. CMake chooses the kernel for
the processor it builds for, and it says which it chose:
`badger: math kernels for 'x64': math_sse2.c`. Any other processor, or a
macOS build for two architectures at once, gets the portable kernel.

Intrinsics headers (`<xmmintrin.h>`, `<arm_neon.h>` and the rest) appear only
in the kernel files; [the foundation check](development.md#the-foundation-check)
holds every other file to that.

`tb_mat4_transform_points()` and `tb_mat4_transform_many()` transform a whole
array in one call, which is where SIMD pays most: a skinned mesh's vertices,
a particle system, or a set of bounds.

## Determinism

The same build on the same platform gives the same results, bit for bit, as
the [vision](../AGENTS.md) promises for replays and lockstep:

- **No fast-math.** The compiler keeps every operation as written.
- **No fused multiply-add.** GCC and Clang build with `-ffp-contract=off`,
  and MSVC with `/fp:precise`, which fuses nothing unless `/fp:contract` asks.
  A fused multiply-add rounds once where the separate operations round twice,
  so allowing it would let the result depend on what the compiler chose.
- **The SIMD and portable kernels agree bit for bit.** They multiply and add
  in the same order, and the tests compare them over many random matrices.

The inline functions are compiled with the settings of the code that calls
them. A game that wants the same guarantee builds with the same flags.

Results may differ between platforms: the C library's `sinf`, `cosf` and
`acosf` are not the same everywhere. Cross-platform determinism is not a goal
yet.

## Edge cases

- Normalizing a vector or quaternion of length 0 returns it unchanged, never
  NaN.
- A function that can have no answer returns false and leaves its output
  alone: `tb_mat4_inverse()` of a singular matrix, `tb_mat4_look_at()` with
  the eye on the target, `tb_plane_from_points()` of points on one line, and
  rays that miss.
- A caller that breaks a documented rule, such as a field of view outside
  (0, π) or an axis of length 0, fails an assertion in a build with
  assertions.
- `tb_nearly_equal()` compares with a tolerance that is absolute near zero
  and relative for large values; it is the comparison for results of float
  arithmetic.

## Geometry

| Shape | Holds | Tests |
| --- | --- | --- |
| `tb_plane` | A unit `normal` facing the front, and its `distance` from the origin | Signed distance; made from a point and normal, or from three counterclockwise points |
| `tb_aabb` | The `minimum` and `maximum` corners | Points, overlap, union, center, extents, and the bounds after a transform |
| `tb_sphere` | A `center` and a `radius` | Points, spheres, and boxes |
| `tb_ray` | An `origin` and a `direction` of any length | Where it first meets a plane, box or sphere; 0 when it starts inside |
| `tb_frustum` | Six planes facing inward, from a view-projection matrix | Points; spheres and boxes as outside, intersecting or inside |

A frustum made from a projection with no far plane has a far plane with a
normal of 0, and everything ahead is in front of it.

## Benchmarks

`tb_bench math` times matrix products and inverses, point transforms in
batches and one call at a time, slerp, and frustum culling. See
[Benchmarks](benchmarks.md).
