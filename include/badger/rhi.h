/** @file rhi.h
 * @brief The render hardware interface: one interface to every graphics API.
 *
 * The RHI is what the renderer draws through. It is shaped after the explicit
 * APIs (D3D12, Vulkan and Metal), as Unreal's RHI is: a device, resources,
 * pipelines, command lists that any thread records, and submissions the GPU
 * runs in order. Each backend implements it behind a function table, and the
 * renderer never names a graphics API.
 *
 * @code
 * tb_rhi_device *device = NULL;
 * tb_rhi_error error = TB_RHI_ERROR_INIT;
 * const tb_rhi_device_options options = {.backend = TB_RHI_BACKEND_D3D12};
 * if (tb_rhi_device_create(&options, &device, &error) != TB_RHI_OK)
 * {
 *     report(error.message);
 * }
 * tb_rhi_swap_chain *chain = NULL;
 * (void)tb_rhi_swap_chain_create(device, window, &(tb_rhi_swap_chain_desc){.vsync = true},
 *                                &chain, &error);
 *
 * tb_rhi_command_list *list = tb_rhi_command_list_begin(device);
 * const tb_rhi_texture target = tb_rhi_swap_chain_current(chain);
 * tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = target,
 *                                            .before = TB_RHI_STATE_PRESENT,
 *                                            .after = TB_RHI_STATE_RENDER_TARGET});
 * tb_rhi_cmd_begin_pass(list, &(tb_rhi_pass){
 *     .color_count = 1,
 *     .colors = {{.texture = target, .load = TB_RHI_LOAD_CLEAR, .clear = {0, 0, 0, 1}}}});
 * ...
 * tb_rhi_cmd_end_pass(list);
 * tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = target,
 *                                            .before = TB_RHI_STATE_RENDER_TARGET,
 *                                            .after = TB_RHI_STATE_PRESENT});
 * (void)tb_rhi_submit(device, &list, 1);
 * (void)tb_rhi_swap_chain_present(chain);
 * @endcode
 *
 * **Handles.** Buffers, textures, samplers, shaders and pipelines are named
 * by handles, not pointers. The RHI keeps a description of every object
 * apart from any backend, so a later change can rebuild them all on another
 * backend, and a game's handles stay valid across the switch. A handle whose
 * object is destroyed is stale, and using it is a contract violation caught
 * in every build. The zero handle names nothing.
 *
 * **Bindless resources.** Shaders reach resources through indices, as
 * Shader Model 6.6 does with `ResourceDescriptorHeap` and
 * `SamplerDescriptorHeap`. Every buffer, texture and sampler has an index for
 * each way a shader reads or writes it (tb_rhi_texture_index() and its
 * peers), given when it is created and kept until it is destroyed. A draw
 * gives its indices to its shaders in push constants (register `b0`, up to
 * #TB_RHI_PUSH_CONSTANT_BYTES), or in constant buffers bound at `b1` and `b2`.
 * There are no binding layouts to declare: every pipeline shares one.
 *
 * **Submissions.** tb_rhi_submit() hands recorded command lists to the GPU
 * and returns a number that grows with every submission. tb_rhi_completed()
 * says how far the GPU has come, and tb_rhi_wait() waits for a number.
 *
 * **Lifetimes.** Destroying an object takes its handle away at once; the
 * backend's object goes when the GPU has finished every submission made up
 * to then. A caller destroys an object only once the command lists that use
 * it are submitted.
 *
 * **States.** A command list states each resource's change of state in a
 * barrier, before and after, as D3D12's and Vulkan's do; the render graph
 * works out which. A new resource starts in #TB_RHI_STATE_COMMON, and a swap
 * chain's image in #TB_RHI_STATE_PRESENT.
 *
 * **Missing features** are capabilities, never failures: tb_rhi_device_info()
 * says what a device has, and a caller falls back.
 *
 * @par Threading
 * Thread-safe: every function but these may be called from any thread.
 * Each command list is externally synchronized: one thread records it at a
 * time, and many threads record different lists at once. A swap chain is
 * stage-owned by the thread that presents it, and tb_rhi_device_destroy() by
 * the thread that created the device, when no other thread uses it.
 */
#ifndef TB_RHI_H
#define TB_RHI_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_log;
struct tb_window;

/** @brief A device: one GPU, through one backend. Created by
 * tb_rhi_device_create(); owned by the caller; destroyed by
 * tb_rhi_device_destroy(). */
typedef struct tb_rhi_device tb_rhi_device;

/** @brief A swap chain: the images a window shows. Created by
 * tb_rhi_swap_chain_create(); owned by the caller; destroyed by
 * tb_rhi_swap_chain_destroy() before its device and its window. */
typedef struct tb_rhi_swap_chain tb_rhi_swap_chain;

/** @brief A list of commands being recorded. Begun by
 * tb_rhi_command_list_begin(); owned by its device; given back by
 * tb_rhi_submit() or tb_rhi_command_list_discard(). */
typedef struct tb_rhi_command_list tb_rhi_command_list;

/** @brief A buffer. Zero names none. */
typedef struct tb_rhi_buffer
{
    uint32_t index;      /**< The slot of the object; private. */
    uint32_t generation; /**< Which object of the slot; private. Zero names none. */
} tb_rhi_buffer;

/** @brief A texture. Zero names none. */
typedef struct tb_rhi_texture
{
    uint32_t index;      /**< The slot of the object; private. */
    uint32_t generation; /**< Which object of the slot; private. Zero names none. */
} tb_rhi_texture;

/** @brief A sampler. Zero names none. */
typedef struct tb_rhi_sampler
{
    uint32_t index;      /**< The slot of the object; private. */
    uint32_t generation; /**< Which object of the slot; private. Zero names none. */
} tb_rhi_sampler;

/** @brief A shader: the bytes one backend runs for one entry point. Zero
 * names none. */
typedef struct tb_rhi_shader
{
    uint32_t index;      /**< The slot of the object; private. */
    uint32_t generation; /**< Which object of the slot; private. Zero names none. */
} tb_rhi_shader;

/** @brief A pipeline: shaders and the fixed state they run with. Zero names
 * none. */
typedef struct tb_rhi_pipeline
{
    uint32_t index;      /**< The slot of the object; private. */
    uint32_t generation; /**< Which object of the slot; private. Zero names none. */
} tb_rhi_pipeline;

/** @brief The graphics API a device draws through. The values are those of
 * the `graphics.renderer` setting's choices. */
typedef enum tb_rhi_backend
{
    TB_RHI_BACKEND_AUTOMATIC, /**< The platform's best that is available. */
    TB_RHI_BACKEND_D3D12,     /**< Direct3D 12, on Windows. */
    TB_RHI_BACKEND_D3D11,     /**< Direct3D 11, on Windows. */
    TB_RHI_BACKEND_VULKAN,    /**< Vulkan, on Windows and Linux. */
    TB_RHI_BACKEND_OPENGL,    /**< OpenGL, everywhere. */
    TB_RHI_BACKEND_METAL,     /**< Metal, on macOS. */
    TB_RHI_BACKEND_SOFTWARE,  /**< The engine's own, on the CPU. */
    /** Accepts every call and draws nothing, as Unreal's null RHI does: for
     * servers, tools, and tests of what sits above the RHI. */
    TB_RHI_BACKEND_NULL,
    TB_RHI_BACKEND_COUNT /**< How many backends there are. */
} tb_rhi_backend;

/** @brief What a call came to. */
typedef enum tb_rhi_result
{
    TB_RHI_OK,            /**< Done. */
    TB_RHI_UNAVAILABLE,   /**< The backend, its library, or a GPU it needs is not there. A
                             capability, not a fault; the message says why. */
    TB_RHI_OUT_OF_MEMORY, /**< The GPU's memory, or the system's, ran out. */
    TB_RHI_DEVICE_LOST,   /**< The device was removed or reset; make a new one. */
    TB_RHI_SYSTEM_ERROR   /**< The graphics API refused; the message says how. */
} tb_rhi_result;

/** @brief Bytes of an error's message, its NUL included. */
#define TB_RHI_MESSAGE_CAPACITY 256

/** @brief Why a call failed. */
typedef struct tb_rhi_error
{
    tb_rhi_result result; /**< As the function returned. */
    /** What went wrong, in UTF-8: "d3d12.dll: no device supports feature level 12_0". */
    char message[TB_RHI_MESSAGE_CAPACITY];
} tb_rhi_error;

/** @brief An error before a call. */
#define TB_RHI_ERROR_INIT {TB_RHI_OK, {0}}

/** @brief Bytes of push constants a draw or dispatch gives its shaders. */
#define TB_RHI_PUSH_CONSTANT_BYTES 128u
/** @brief Constant buffers a command list binds, at `b1` and up. */
#define TB_RHI_CONSTANT_BUFFER_SLOTS 2u
/** @brief Color targets of a pass or a pipeline. */
#define TB_RHI_COLOR_TARGETS 8u
/** @brief Vertex buffers a pipeline reads. */
#define TB_RHI_VERTEX_BUFFERS 8u
/** @brief Vertex attributes a pipeline reads. */
#define TB_RHI_VERTEX_ATTRIBUTES 16u
/** @brief Bytes of a debug name, its NUL included. */
#define TB_RHI_NAME_CAPACITY 64u
/** @brief A bindless index that names nothing: what a resource that cannot
 * be read or written that way gives. */
#define TB_RHI_NO_INDEX 0xFFFFFFFFu

/** @brief Which GPU a device prefers. */
typedef enum tb_rhi_adapter_preference
{
    TB_RHI_ADAPTER_HIGH_PERFORMANCE, /**< The fastest GPU, a discrete one where there is. */
    TB_RHI_ADAPTER_LOW_POWER,        /**< The GPU that saves power, an integrated one. */
    TB_RHI_ADAPTER_SOFTWARE          /**< The system's GPU on the CPU, such as WARP. */
} tb_rhi_adapter_preference;

/** @brief How to make a device. Zero-initialize, then set what differs. */
typedef struct tb_rhi_device_options
{
    tb_rhi_backend backend;            /**< Which; #TB_RHI_BACKEND_AUTOMATIC by default. */
    tb_rhi_adapter_preference adapter; /**< Which GPU. */
    /** Turn on the graphics API's validation, such as the D3D12 debug layer,
     * and route its messages to the log. */
    bool validation;
    /** Resources that shaders reach by index at once; 0 is 65536. */
    uint32_t bindless_resources;
    /** Samplers that shaders reach by index at once; 0 is 2048. */
    uint32_t bindless_samplers;
    /** Where the device says which backend it chose and why it passed over
     * others, and where the graphics API's validation messages go, all in
     * category "rhi"; NULL drops them. Borrowed until the device is
     * destroyed. */
    struct tb_log *log;
} tb_rhi_device_options;

/** @brief Features a device may have, as bits of tb_rhi_device_info::features. */
typedef enum tb_rhi_feature
{
    TB_RHI_FEATURE_BINDLESS = 1u << 0,          /**< Shaders index every resource at once. */
    TB_RHI_FEATURE_RAY_QUERY = 1u << 1,         /**< Inline ray tracing (DXR 1.1). */
    TB_RHI_FEATURE_RAY_PIPELINE = 1u << 2,      /**< Ray-tracing pipelines. */
    TB_RHI_FEATURE_MESH_SHADER = 1u << 3,       /**< Mesh and amplification shaders. */
    TB_RHI_FEATURE_ENHANCED_BARRIERS = 1u << 4, /**< D3D12's enhanced barriers. */
    TB_RHI_FEATURE_TEARING = 1u << 5,           /**< Presenting without vsync may tear. */
    TB_RHI_FEATURE_TIMESTAMPS = 1u << 6         /**< GPU timestamps. */
} tb_rhi_feature;

/** @brief Bytes of an adapter's name, its NUL included. */
#define TB_RHI_ADAPTER_NAME_CAPACITY 128u

/** @brief What a device is and has. */
typedef struct tb_rhi_device_info
{
    tb_rhi_backend backend;                     /**< Never automatic. */
    char adapter[TB_RHI_ADAPTER_NAME_CAPACITY]; /**< "NVIDIA GeForce RTX 4090", in UTF-8. */
    uint32_t vendor;                            /**< The PCI vendor: 0x10DE NVIDIA, 0x1002
                                                   AMD, 0x8086 Intel; 0 if unknown. */
    bool software;                              /**< The GPU runs on the CPU. */
    uint64_t video_memory;                      /**< Bytes of the GPU's own memory. */
    uint32_t features;                          /**< Bits of #tb_rhi_feature. */
    uint32_t shader_model;                      /**< The highest, as 66 for 6.6; 0 if none. */
    uint32_t max_texture_size;                  /**< Texels on a side of a 2D texture. */
    uint32_t bindless_resources;                /**< As made; see the options. */
    uint32_t bindless_samplers;                 /**< As made; see the options. */
    uint64_t timestamp_frequency;               /**< Ticks a second of timestamps. */
} tb_rhi_device_info;

/** @brief The layout of a texture's or a vertex's elements. */
typedef enum tb_rhi_format
{
    TB_RHI_FORMAT_UNKNOWN,            /**< None. */
    TB_RHI_FORMAT_R8_UNORM,           /**< One 8-bit channel. */
    TB_RHI_FORMAT_R8G8_UNORM,         /**< Two 8-bit channels. */
    TB_RHI_FORMAT_R8G8B8A8_UNORM,     /**< Four 8-bit channels. */
    TB_RHI_FORMAT_R8G8B8A8_SRGB,      /**< Four 8-bit channels, color in sRGB. */
    TB_RHI_FORMAT_B8G8R8A8_UNORM,     /**< Four 8-bit channels, blue first: swap chains. */
    TB_RHI_FORMAT_B8G8R8A8_SRGB,      /**< As above, color in sRGB. */
    TB_RHI_FORMAT_R10G10B10A2_UNORM,  /**< 10-bit color and 2-bit alpha: HDR swap chains. */
    TB_RHI_FORMAT_R11G11B10_FLOAT,    /**< Packed floating-point color. */
    TB_RHI_FORMAT_R16_FLOAT,          /**< One half. */
    TB_RHI_FORMAT_R16G16_FLOAT,       /**< Two halves. */
    TB_RHI_FORMAT_R16G16B16A16_FLOAT, /**< Four halves. */
    TB_RHI_FORMAT_R16_UINT,           /**< One 16-bit unsigned integer: indices. */
    TB_RHI_FORMAT_R32_UINT,           /**< One 32-bit unsigned integer: indices. */
    TB_RHI_FORMAT_R32_FLOAT,          /**< One float. */
    TB_RHI_FORMAT_R32G32_FLOAT,       /**< Two floats. */
    TB_RHI_FORMAT_R32G32B32_FLOAT,    /**< Three floats: vertex positions. */
    TB_RHI_FORMAT_R32G32B32A32_FLOAT, /**< Four floats. */
    TB_RHI_FORMAT_D32_FLOAT,          /**< Depth, as a float. */
    TB_RHI_FORMAT_D24_UNORM_S8_UINT,  /**< Depth and stencil. */
    TB_RHI_FORMAT_COUNT               /**< How many formats there are. */
} tb_rhi_format;

/** @brief Bytes of one element of a format.
 * @return 0 for #TB_RHI_FORMAT_UNKNOWN or a value out of range.
 * @par Threading Thread-safe. */
TB_API uint32_t tb_rhi_format_size(tb_rhi_format format);

/** @brief Whether a format holds depth.
 * @par Threading Thread-safe. */
TB_API bool tb_rhi_format_is_depth(tb_rhi_format format);

/** @brief Where a buffer's memory is. */
typedef enum tb_rhi_memory
{
    TB_RHI_MEMORY_GPU,     /**< The GPU's own: fast to it; filled by copies. */
    TB_RHI_MEMORY_UPLOAD,  /**< Written by the CPU, read by the GPU: staging and data that
                              changes every frame. */
    TB_RHI_MEMORY_READBACK /**< Written by the GPU, read by the CPU. */
} tb_rhi_memory;

/** @brief What a buffer is used for, as bits. */
typedef enum tb_rhi_buffer_usage
{
    TB_RHI_BUFFER_VERTEX = 1u << 0,      /**< Vertex buffer. */
    TB_RHI_BUFFER_INDEX = 1u << 1,       /**< Index buffer. */
    TB_RHI_BUFFER_CONSTANT = 1u << 2,    /**< Constant buffer, bound at `b1` and up. */
    TB_RHI_BUFFER_SHADER_READ = 1u << 3, /**< Read by index, as a ByteAddressBuffer. */
    TB_RHI_BUFFER_STORAGE = 1u << 4,     /**< Written by index, as a RWByteAddressBuffer. */
    TB_RHI_BUFFER_INDIRECT = 1u << 5     /**< Arguments of indirect draws. */
} tb_rhi_buffer_usage;

/** @brief A buffer to make. */
typedef struct tb_rhi_buffer_desc
{
    uint64_t size;        /**< Bytes; more than 0. */
    uint32_t usage;       /**< Bits of #tb_rhi_buffer_usage. Copies need no bit. */
    tb_rhi_memory memory; /**< Where. */
    const char *name;     /**< For debuggers; may be NULL; copied. */
} tb_rhi_buffer_desc;

/** @brief The shape of a texture. */
typedef enum tb_rhi_texture_kind
{
    TB_RHI_TEXTURE_2D,  /**< A 2D image, or an array of them. */
    TB_RHI_TEXTURE_CUBE /**< Six square images; `layers` is a multiple of 6. */
} tb_rhi_texture_kind;

/** @brief What a texture is used for, as bits. */
typedef enum tb_rhi_texture_usage
{
    TB_RHI_TEXTURE_SAMPLED = 1u << 0,       /**< Read by shaders. */
    TB_RHI_TEXTURE_STORAGE = 1u << 1,       /**< Written by shaders. */
    TB_RHI_TEXTURE_RENDER_TARGET = 1u << 2, /**< A pass's color target. */
    TB_RHI_TEXTURE_DEPTH = 1u << 3          /**< A pass's depth target. */
} tb_rhi_texture_usage;

/** @brief A texture to make. */
typedef struct tb_rhi_texture_desc
{
    tb_rhi_texture_kind kind; /**< Shape. */
    tb_rhi_format format;     /**< Layout of a texel. */
    uint32_t width;           /**< Texels; more than 0. */
    uint32_t height;          /**< Texels; more than 0. */
    uint32_t layers;          /**< Images; 0 is 1 (6 for a cube). */
    uint32_t mips;            /**< Levels; 0 is 1. */
    uint32_t samples;         /**< Per texel; 0 is 1. */
    uint32_t usage;           /**< Bits of #tb_rhi_texture_usage. */
    float clear[4];           /**< The clear most used, which some GPUs make faster. */
    const char *name;         /**< For debuggers; may be NULL; copied. */
} tb_rhi_texture_desc;

/** @brief How a sampler filters. */
typedef enum tb_rhi_filter
{
    TB_RHI_FILTER_LINEAR, /**< Blend nearby texels. */
    TB_RHI_FILTER_NEAREST /**< Take the nearest: pixel art. */
} tb_rhi_filter;

/** @brief What a sampler reads outside a texture. */
typedef enum tb_rhi_address
{
    TB_RHI_ADDRESS_REPEAT, /**< Tile. */
    TB_RHI_ADDRESS_MIRROR, /**< Tile, mirrored. */
    TB_RHI_ADDRESS_CLAMP,  /**< The edge. */
    TB_RHI_ADDRESS_BORDER  /**< Transparent black. */
} tb_rhi_address;

/** @brief A comparison of depth or of a sampled value. */
typedef enum tb_rhi_compare
{
    TB_RHI_COMPARE_NEVER,         /**< Never passes. */
    TB_RHI_COMPARE_LESS,          /**< New < old. */
    TB_RHI_COMPARE_EQUAL,         /**< New == old. */
    TB_RHI_COMPARE_LESS_EQUAL,    /**< New <= old. */
    TB_RHI_COMPARE_GREATER,       /**< New > old: reversed depth. */
    TB_RHI_COMPARE_NOT_EQUAL,     /**< New != old. */
    TB_RHI_COMPARE_GREATER_EQUAL, /**< New >= old. */
    TB_RHI_COMPARE_ALWAYS         /**< Always passes. */
} tb_rhi_compare;

/** @brief A sampler to make. Zero is trilinear, repeating, without
 * comparison. */
typedef struct tb_rhi_sampler_desc
{
    tb_rhi_filter min_filter;  /**< Minification. */
    tb_rhi_filter mag_filter;  /**< Magnification. */
    tb_rhi_filter mip_filter;  /**< Between levels. */
    tb_rhi_address address_u;  /**< Across. */
    tb_rhi_address address_v;  /**< Down. */
    tb_rhi_address address_w;  /**< Deep, or across a cube's faces. */
    uint32_t anisotropy;       /**< 0 or 1 is none; up to 16. */
    bool compare;              /**< A comparison sampler, for shadows. */
    tb_rhi_compare compare_op; /**< With `compare`. */
    float mip_bias;            /**< Added to the level. */
    const char *name;          /**< For debuggers; may be NULL; copied. */
} tb_rhi_sampler_desc;

/** @brief The stage a shader runs at. */
typedef enum tb_rhi_stage
{
    TB_RHI_STAGE_VERTEX, /**< Vertices. */
    TB_RHI_STAGE_PIXEL,  /**< Pixels. */
    TB_RHI_STAGE_COMPUTE /**< Compute. */
} tb_rhi_stage;

/** @brief A shader to make, from what the shader compiler made for this
 * device's backend: DXIL for D3D12. */
typedef struct tb_rhi_shader_desc
{
    tb_rhi_stage stage; /**< Its stage. */
    const void *code;   /**< The backend's bytes; copied. */
    size_t size;        /**< Bytes of `code`; more than 0. */
    const char *entry;  /**< The entry point's name; NULL is "main"; copied. */
    const char *name;   /**< For debuggers; may be NULL; copied. */
} tb_rhi_shader_desc;

/** @brief Which way a vertex attribute advances. */
typedef enum tb_rhi_step
{
    TB_RHI_STEP_VERTEX,  /**< Each vertex. */
    TB_RHI_STEP_INSTANCE /**< Each instance. */
} tb_rhi_step;

/** @brief One attribute of a vertex. */
typedef struct tb_rhi_vertex_attribute
{
    const char *semantic;    /**< The shader's semantic: "POSITION"; borrowed for the call. */
    uint32_t semantic_index; /**< As in "TEXCOORD1". */
    tb_rhi_format format;    /**< Its layout. */
    uint32_t buffer;         /**< Which vertex buffer. */
    uint32_t offset;         /**< Bytes into a vertex. */
} tb_rhi_vertex_attribute;

/** @brief One vertex buffer of a pipeline. */
typedef struct tb_rhi_vertex_buffer_layout
{
    uint32_t stride;  /**< Bytes from a vertex to the next. */
    tb_rhi_step step; /**< Per vertex or per instance. */
} tb_rhi_vertex_buffer_layout;

/** @brief What a pipeline draws. */
typedef enum tb_rhi_topology
{
    TB_RHI_TOPOLOGY_TRIANGLES,      /**< Each three vertices. */
    TB_RHI_TOPOLOGY_TRIANGLE_STRIP, /**< Each vertex with the two before. */
    TB_RHI_TOPOLOGY_LINES,          /**< Each two vertices. */
    TB_RHI_TOPOLOGY_LINE_STRIP,     /**< Each vertex with the one before. */
    TB_RHI_TOPOLOGY_POINTS          /**< Each vertex. */
} tb_rhi_topology;

/** @brief Which faces a pipeline leaves out. */
typedef enum tb_rhi_cull
{
    TB_RHI_CULL_NONE, /**< None. */
    TB_RHI_CULL_BACK, /**< Faces away. */
    TB_RHI_CULL_FRONT /**< Faces toward. */
} tb_rhi_cull;

/** @brief A factor of blending. */
typedef enum tb_rhi_blend_factor
{
    TB_RHI_BLEND_ZERO,                   /**< 0. */
    TB_RHI_BLEND_ONE,                    /**< 1. */
    TB_RHI_BLEND_SOURCE_COLOR,           /**< The new color. */
    TB_RHI_BLEND_ONE_MINUS_SOURCE_COLOR, /**< 1 - the new color. */
    TB_RHI_BLEND_SOURCE_ALPHA,           /**< The new alpha. */
    TB_RHI_BLEND_ONE_MINUS_SOURCE_ALPHA, /**< 1 - the new alpha. */
    TB_RHI_BLEND_DEST_COLOR,             /**< The old color. */
    TB_RHI_BLEND_ONE_MINUS_DEST_COLOR,   /**< 1 - the old color. */
    TB_RHI_BLEND_DEST_ALPHA,             /**< The old alpha. */
    TB_RHI_BLEND_ONE_MINUS_DEST_ALPHA    /**< 1 - the old alpha. */
} tb_rhi_blend_factor;

/** @brief How blending combines. */
typedef enum tb_rhi_blend_op
{
    TB_RHI_BLEND_ADD,              /**< Source + dest. */
    TB_RHI_BLEND_SUBTRACT,         /**< Source - dest. */
    TB_RHI_BLEND_REVERSE_SUBTRACT, /**< Dest - source. */
    TB_RHI_BLEND_MIN,              /**< The smaller. */
    TB_RHI_BLEND_MAX               /**< The larger. */
} tb_rhi_blend_op;

/** @brief Blending of one color target. Zero writes without blending. */
typedef struct tb_rhi_blend
{
    bool enabled;                     /**< Blend at all. */
    tb_rhi_blend_factor color_source; /**< Factor of the new color. */
    tb_rhi_blend_factor color_dest;   /**< Factor of the old color. */
    tb_rhi_blend_op color_op;         /**< How they combine. */
    tb_rhi_blend_factor alpha_source; /**< Factor of the new alpha. */
    tb_rhi_blend_factor alpha_dest;   /**< Factor of the old alpha. */
    tb_rhi_blend_op alpha_op;         /**< How they combine. */
    /** Channels left unwritten, as bits: 1 red, 2 green, 4 blue, 8 alpha.
     * Zero writes all, so that a zeroed blend writes. */
    uint32_t write_mask_off;
} tb_rhi_blend;

/** @brief A graphics pipeline to make. */
typedef struct tb_rhi_graphics_pipeline_desc
{
    tb_rhi_shader vertex;                      /**< Required. */
    tb_rhi_shader pixel;                       /**< May be none, for depth alone. */
    const tb_rhi_vertex_attribute *attributes; /**< Borrowed for the call. */
    uint32_t attribute_count; /**< Up to #TB_RHI_VERTEX_ATTRIBUTES; 0 pulls vertices by index. */
    tb_rhi_vertex_buffer_layout buffers[TB_RHI_VERTEX_BUFFERS]; /**< The vertex buffers. */
    uint32_t buffer_count;                                      /**< How many of `buffers`. */
    tb_rhi_topology topology;                                   /**< What it draws. */
    tb_rhi_cull cull;                                           /**< Which faces it leaves out. */
    bool clockwise_front;         /**< Faces wound clockwise face toward; else counter-clockwise. */
    bool wireframe;               /**< Draw edges alone. */
    float depth_bias;             /**< Constant bias of depth, for shadows. */
    float depth_bias_slope;       /**< Bias by slope. */
    bool depth_test;              /**< Test depth. */
    bool depth_write;             /**< Write depth. */
    tb_rhi_compare depth_compare; /**< How depth is tested. */
    tb_rhi_blend blend[TB_RHI_COLOR_TARGETS];   /**< Per color target. */
    tb_rhi_format colors[TB_RHI_COLOR_TARGETS]; /**< The targets' formats. */
    uint32_t color_count;                       /**< How many of `colors`. */
    tb_rhi_format depth; /**< The depth target's format, or unknown for none. */
    uint32_t samples;    /**< Per pixel; 0 is 1. */
    const char *name;    /**< For debuggers; may be NULL; copied. */
} tb_rhi_graphics_pipeline_desc;

/** @brief A compute pipeline to make. */
typedef struct tb_rhi_compute_pipeline_desc
{
    tb_rhi_shader compute; /**< Required. */
    const char *name;      /**< For debuggers; may be NULL; copied. */
} tb_rhi_compute_pipeline_desc;

/** @brief A swap chain to make. */
typedef struct tb_rhi_swap_chain_desc
{
    tb_rhi_format format; /**< Unknown is #TB_RHI_FORMAT_B8G8R8A8_UNORM. */
    uint32_t images;      /**< Images to cycle; 0 is 3, at least 2. */
    bool vsync;           /**< Wait for the display's refresh. */
} tb_rhi_swap_chain_desc;

/** @brief The state of a resource, which barriers change. */
typedef enum tb_rhi_state
{
    TB_RHI_STATE_COMMON,        /**< Where a resource starts, and copies across queues. */
    TB_RHI_STATE_VERTEX_INDEX,  /**< Read as vertices or indices. */
    TB_RHI_STATE_CONSTANT,      /**< Read as constants. */
    TB_RHI_STATE_SHADER_READ,   /**< Read by shaders by index. */
    TB_RHI_STATE_STORAGE,       /**< Written by shaders. */
    TB_RHI_STATE_RENDER_TARGET, /**< A color target. */
    TB_RHI_STATE_DEPTH_WRITE,   /**< A depth target, written. */
    TB_RHI_STATE_DEPTH_READ,    /**< A depth target, tested only, or read. */
    TB_RHI_STATE_COPY_SOURCE,   /**< Copied from. */
    TB_RHI_STATE_COPY_DEST,     /**< Copied to. */
    TB_RHI_STATE_INDIRECT,      /**< Arguments of indirect draws. */
    TB_RHI_STATE_PRESENT        /**< Shown by a swap chain. */
} tb_rhi_state;

/** @brief A change of one resource's state. Name a buffer or a texture. */
typedef struct tb_rhi_barrier
{
    tb_rhi_buffer buffer;   /**< The buffer, or none. */
    tb_rhi_texture texture; /**< The texture, or none. */
    tb_rhi_state before;    /**< Its state until now. */
    tb_rhi_state after;     /**< Its state from now. */
} tb_rhi_barrier;

/** @brief What a pass does with a target's contents first. */
typedef enum tb_rhi_load
{
    TB_RHI_LOAD_KEEP,  /**< Keep them. */
    TB_RHI_LOAD_CLEAR, /**< Clear them. */
    TB_RHI_LOAD_ANY    /**< Whatever is cheapest: every pixel is drawn. */
} tb_rhi_load;

/** @brief One color target of a pass. */
typedef struct tb_rhi_color_target
{
    tb_rhi_texture texture; /**< In #TB_RHI_STATE_RENDER_TARGET. */
    uint32_t mip;           /**< Its level. */
    uint32_t layer;         /**< Its image. */
    tb_rhi_load load;       /**< What first. */
    float clear[4];         /**< With #TB_RHI_LOAD_CLEAR. */
} tb_rhi_color_target;

/** @brief The depth target of a pass. */
typedef struct tb_rhi_depth_target
{
    tb_rhi_texture texture; /**< In a depth state, or none. */
    tb_rhi_load load;       /**< What first. */
    float clear;            /**< With #TB_RHI_LOAD_CLEAR. */
    uint8_t stencil_clear;  /**< With #TB_RHI_LOAD_CLEAR. */
    bool read_only;         /**< Test without writing. */
} tb_rhi_depth_target;

/** @brief A pass: the targets that draws between tb_rhi_cmd_begin_pass() and
 * tb_rhi_cmd_end_pass() write. The viewport and scissor start as the first
 * target's size. */
typedef struct tb_rhi_pass
{
    tb_rhi_color_target colors[TB_RHI_COLOR_TARGETS]; /**< The color targets. */
    uint32_t color_count;                             /**< How many of `colors`. */
    tb_rhi_depth_target depth;                        /**< The depth target. */
} tb_rhi_pass;

/** @brief A region of a target, in pixels from its top left. */
typedef struct tb_rhi_rect
{
    int32_t x;       /**< Left. */
    int32_t y;       /**< Top. */
    uint32_t width;  /**< Across. */
    uint32_t height; /**< Down. */
} tb_rhi_rect;

/** @brief A copy between a buffer and a texture's image. */
typedef struct tb_rhi_texture_copy
{
    tb_rhi_buffer buffer;   /**< The buffer. */
    uint64_t offset;        /**< Bytes into it; a multiple of 512. */
    uint32_t row_pitch;     /**< Bytes from a row to the next; a multiple of 256. */
    tb_rhi_texture texture; /**< The texture. */
    uint32_t mip;           /**< Its level. */
    uint32_t layer;         /**< Its image. */
    tb_rhi_rect region;     /**< Its texels; zero size is the whole level. */
} tb_rhi_texture_copy;

/** @brief Alignment of a texture copy's offset into a buffer. */
#define TB_RHI_TEXTURE_COPY_OFFSET_ALIGNMENT 512u
/** @brief Alignment of a texture copy's row pitch. */
#define TB_RHI_TEXTURE_COPY_PITCH_ALIGNMENT 256u
/** @brief Alignment of a constant buffer's offset. */
#define TB_RHI_CONSTANT_ALIGNMENT 256u

/**
 * @brief Make a device on the backend the options name, as the
 * `graphics.renderer` setting chooses it.
 *
 * #TB_RHI_BACKEND_AUTOMATIC tries the platform's backends in its order of
 * preference (Windows: D3D12, D3D11, Vulkan, OpenGL, software; Linux: Vulkan,
 * OpenGL, software; macOS: Metal, OpenGL, software), passes over any that is
 * not written yet or is unavailable, and takes the first that makes a device.
 * It never takes the null backend. A backend named outright is the only one
 * tried. Either way, the options' log says which backend was used, and why
 * each other was passed over.
 *
 * @param options Required non-null.
 * @param device Required non-null; set to the device, or to NULL on failure.
 * @param error May be NULL; on failure, says why.
 * @return #TB_RHI_OK, or why there is no device: #TB_RHI_UNAVAILABLE where
 * the backend named, or every backend `auto` tried, is unavailable on this
 * machine. A backend this platform does not build, or that is not written
 * yet, named outright, is not a failure: it ends the process through
 * tb_fatal(), saying "not implemented", the platform, and the next step:
 * set `graphics.renderer` to `auto`, or run with `--renderer=auto`.
 * @par Threading Thread-safe.
 */
TB_API tb_rhi_result tb_rhi_device_create(const tb_rhi_device_options *options,
                                          tb_rhi_device **device, tb_rhi_error *error);

/**
 * @brief Destroy a device: wait for the GPU, then release everything.
 * @param device May be NULL. Its swap chains must be destroyed already, and
 * no command list may be recording. Objects still alive are released with it.
 * @par Threading Stage-owned: the thread that made it, when no other uses it.
 */
TB_API void tb_rhi_device_destroy(tb_rhi_device *device);

/**
 * @brief What a device is and has.
 * @param device Required non-null.
 * @param info Required non-null; filled.
 */
TB_API void tb_rhi_device_info_of(const tb_rhi_device *device, tb_rhi_device_info *info);

/**
 * @brief The name of a backend, as `graphics.renderer` spells it: "d3d12".
 * @return A static string; "unknown" for a value out of range.
 * @par Threading Thread-safe.
 */
TB_API const char *tb_rhi_backend_name(tb_rhi_backend backend);

/**
 * @brief The name of a result: "TB_RHI_OK".
 * @return A static string; "unknown" for a value out of range.
 * @par Threading Thread-safe.
 */
TB_API const char *tb_rhi_result_name(tb_rhi_result result);

/**
 * @brief Make a buffer.
 * @param device Required non-null.
 * @param desc Required non-null; `size` more than 0.
 * @param buffer Required non-null; set to the buffer, or to none on failure.
 * @param error May be NULL; on failure, says why.
 */
TB_API tb_rhi_result tb_rhi_buffer_create(tb_rhi_device *device, const tb_rhi_buffer_desc *desc,
                                          tb_rhi_buffer *buffer, tb_rhi_error *error);

/** @brief Destroy a buffer; see Lifetimes. A none buffer is ignored. */
TB_API void tb_rhi_buffer_destroy(tb_rhi_device *device, tb_rhi_buffer buffer);

/**
 * @brief Write to a buffer of #TB_RHI_MEMORY_UPLOAD.
 * @param data Required non-null; `size` bytes, copied.
 * @param offset Plus `size`, within the buffer.
 * @par Threading Thread-safe; the GPU must not be reading those bytes.
 */
TB_API void tb_rhi_buffer_write(tb_rhi_device *device, tb_rhi_buffer buffer, uint64_t offset,
                                const void *data, size_t size);

/**
 * @brief Read from a buffer of #TB_RHI_MEMORY_READBACK, once the GPU has
 * finished writing it.
 * @param data Required non-null; receives `size` bytes.
 * @param offset Plus `size`, within the buffer.
 */
TB_API void tb_rhi_buffer_read(tb_rhi_device *device, tb_rhi_buffer buffer, uint64_t offset,
                               void *data, size_t size);

/** @brief What a buffer was made with; its name is NULL.
 * @param desc Required non-null; filled. */
TB_API void tb_rhi_buffer_desc_of(tb_rhi_device *device, tb_rhi_buffer buffer,
                                  tb_rhi_buffer_desc *desc);

/** @brief The index by which shaders read a buffer of
 * #TB_RHI_BUFFER_SHADER_READ, or #TB_RHI_NO_INDEX. */
TB_API uint32_t tb_rhi_buffer_index(tb_rhi_device *device, tb_rhi_buffer buffer);

/** @brief The index by which shaders write a buffer of #TB_RHI_BUFFER_STORAGE,
 * or #TB_RHI_NO_INDEX. */
TB_API uint32_t tb_rhi_buffer_storage_index(tb_rhi_device *device, tb_rhi_buffer buffer);

/**
 * @brief Make a texture.
 * @param device Required non-null.
 * @param desc Required non-null.
 * @param texture Required non-null; set to the texture, or to none on failure.
 * @param error May be NULL; on failure, says why.
 */
TB_API tb_rhi_result tb_rhi_texture_create(tb_rhi_device *device, const tb_rhi_texture_desc *desc,
                                           tb_rhi_texture *texture, tb_rhi_error *error);

/** @brief Destroy a texture; see Lifetimes. A none texture is ignored; a swap
 * chain's images are not the caller's to destroy. */
TB_API void tb_rhi_texture_destroy(tb_rhi_device *device, tb_rhi_texture texture);

/** @brief What a texture was made with, its defaults filled in; its name is
 * NULL.
 * @param desc Required non-null; filled. */
TB_API void tb_rhi_texture_desc_of(tb_rhi_device *device, tb_rhi_texture texture,
                                   tb_rhi_texture_desc *desc);

/** @brief The index by which shaders sample a texture of
 * #TB_RHI_TEXTURE_SAMPLED, or #TB_RHI_NO_INDEX. */
TB_API uint32_t tb_rhi_texture_index(tb_rhi_device *device, tb_rhi_texture texture);

/** @brief The index by which shaders write a texture of
 * #TB_RHI_TEXTURE_STORAGE, or #TB_RHI_NO_INDEX. */
TB_API uint32_t tb_rhi_texture_storage_index(tb_rhi_device *device, tb_rhi_texture texture);

/**
 * @brief Make a sampler.
 * @param desc Required non-null.
 * @param sampler Required non-null; set to the sampler, or to none on failure.
 * @param error May be NULL; on failure, says why.
 */
TB_API tb_rhi_result tb_rhi_sampler_create(tb_rhi_device *device, const tb_rhi_sampler_desc *desc,
                                           tb_rhi_sampler *sampler, tb_rhi_error *error);

/** @brief Destroy a sampler; see Lifetimes. A none sampler is ignored. */
TB_API void tb_rhi_sampler_destroy(tb_rhi_device *device, tb_rhi_sampler sampler);

/** @brief The index by which shaders reach a sampler. */
TB_API uint32_t tb_rhi_sampler_index(tb_rhi_device *device, tb_rhi_sampler sampler);

/**
 * @brief Make a shader from bytes made for this device's backend.
 * @param desc Required non-null.
 * @param shader Required non-null; set to the shader, or to none on failure.
 * @param error May be NULL; on failure, says why: bytes the backend rejects
 * are #TB_RHI_SYSTEM_ERROR.
 */
TB_API tb_rhi_result tb_rhi_shader_create(tb_rhi_device *device, const tb_rhi_shader_desc *desc,
                                          tb_rhi_shader *shader, tb_rhi_error *error);

/** @brief Destroy a shader. Pipelines made from it keep working. A none
 * shader is ignored. */
TB_API void tb_rhi_shader_destroy(tb_rhi_device *device, tb_rhi_shader shader);

/**
 * @brief Make a graphics pipeline.
 * @param desc Required non-null; its shaders alive, a vertex and, if any, a
 * pixel shader.
 * @param pipeline Required non-null; set to the pipeline, or to none on failure.
 * @param error May be NULL; on failure, says why.
 */
TB_API tb_rhi_result tb_rhi_graphics_pipeline_create(tb_rhi_device *device,
                                                     const tb_rhi_graphics_pipeline_desc *desc,
                                                     tb_rhi_pipeline *pipeline,
                                                     tb_rhi_error *error);

/**
 * @brief Make a compute pipeline.
 * @param desc Required non-null; its shader alive, a compute shader.
 * @param pipeline Required non-null; set to the pipeline, or to none on failure.
 * @param error May be NULL; on failure, says why.
 */
TB_API tb_rhi_result tb_rhi_compute_pipeline_create(tb_rhi_device *device,
                                                    const tb_rhi_compute_pipeline_desc *desc,
                                                    tb_rhi_pipeline *pipeline, tb_rhi_error *error);

/** @brief Destroy a pipeline; see Lifetimes. A none pipeline is ignored. */
TB_API void tb_rhi_pipeline_destroy(tb_rhi_device *device, tb_rhi_pipeline pipeline);

/**
 * @brief Make a swap chain for a window, sized to its pixels.
 * @param window Required non-null; outlives the swap chain.
 * @param desc Required non-null.
 * @param chain Required non-null; set to the swap chain, or to NULL on failure.
 * @param error May be NULL; on failure, says why.
 * @par Threading Stage-owned: the swap chain belongs to the calling thread.
 */
TB_API tb_rhi_result tb_rhi_swap_chain_create(tb_rhi_device *device, struct tb_window *window,
                                              const tb_rhi_swap_chain_desc *desc,
                                              tb_rhi_swap_chain **chain, tb_rhi_error *error);

/** @brief Destroy a swap chain, waiting for the GPU to finish with it. NULL
 * is ignored.
 * @par Threading Stage-owned. */
TB_API void tb_rhi_swap_chain_destroy(tb_rhi_swap_chain *chain);

/** @brief The image to draw this frame, in #TB_RHI_STATE_PRESENT until a
 * barrier says otherwise.
 * @par Threading Stage-owned. */
TB_API tb_rhi_texture tb_rhi_swap_chain_current(tb_rhi_swap_chain *chain);

/**
 * @brief Show the current image, and move to the next.
 * @return #TB_RHI_OK, or #TB_RHI_DEVICE_LOST.
 * @par Threading Stage-owned.
 */
TB_API tb_rhi_result tb_rhi_swap_chain_present(tb_rhi_swap_chain *chain);

/**
 * @brief Size the images to a window's new size in pixels, or change vsync.
 * Waits for the GPU; the images change, and their handles with them.
 * @param width Pixels; 0 keeps the window's.
 * @param height Pixels; 0 keeps the window's.
 * @param error May be NULL; on failure, says why.
 * @par Threading Stage-owned.
 */
TB_API tb_rhi_result tb_rhi_swap_chain_resize(tb_rhi_swap_chain *chain, uint32_t width,
                                              uint32_t height, bool vsync, tb_rhi_error *error);

/** @brief Begin a command list. It records until submitted or discarded.
 * @return Never NULL. */
TB_API tb_rhi_command_list *tb_rhi_command_list_begin(tb_rhi_device *device);

/** @brief Give back a command list without running it.
 * @par Threading Externally synchronized with the list's recording. */
TB_API void tb_rhi_command_list_discard(tb_rhi_command_list *list);

/**
 * @brief Run command lists on the GPU, in order, after every submission
 * before.
 * @param lists Required non-null; `count` lists of this device, each out of
 * any pass. Each is given back: none may be used again.
 * @return The submission's number: more than any before.
 * @par Threading Thread-safe, as one submission at a time.
 */
TB_API uint64_t tb_rhi_submit(tb_rhi_device *device, tb_rhi_command_list *const *lists,
                              size_t count);

/** @brief The number of the last submission the GPU has finished. */
TB_API uint64_t tb_rhi_completed(tb_rhi_device *device);

/** @brief Wait until the GPU has finished a submission.
 * @return #TB_RHI_OK, or #TB_RHI_DEVICE_LOST. */
TB_API tb_rhi_result tb_rhi_wait(tb_rhi_device *device, uint64_t submission);

/** @brief Wait until the GPU has finished everything submitted.
 * @return #TB_RHI_OK, or #TB_RHI_DEVICE_LOST. */
TB_API tb_rhi_result tb_rhi_wait_idle(tb_rhi_device *device);

/**
 * @brief Change resources' states.
 * @param barriers Required non-null; `count` of them, each naming one buffer
 * or one texture of the device.
 * @par Threading Externally synchronized, as every tb_rhi_cmd_ function is.
 */
TB_API void tb_rhi_cmd_barriers(tb_rhi_command_list *list, const tb_rhi_barrier *barriers,
                                size_t count);

/** @brief Change one resource's state.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_barrier(tb_rhi_command_list *list, const tb_rhi_barrier *barrier);

/**
 * @brief Begin a pass: draws write its targets until tb_rhi_cmd_end_pass().
 * @param pass Required non-null; at least one target, all the same size.
 * @par Threading Externally synchronized.
 */
TB_API void tb_rhi_cmd_begin_pass(tb_rhi_command_list *list, const tb_rhi_pass *pass);

/** @brief End the pass.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_end_pass(tb_rhi_command_list *list);

/** @brief Draw into a region of the targets, with depth from `near` to `far`.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_viewport(tb_rhi_command_list *list, float x, float y, float width,
                                float height, float near_depth, float far_depth);

/** @brief Draw only inside a region of the targets.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_scissor(tb_rhi_command_list *list, tb_rhi_rect rect);

/** @brief Use a pipeline for the draws or dispatches after.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_pipeline(tb_rhi_command_list *list, tb_rhi_pipeline pipeline);

/**
 * @brief Give the shaders of the draws or dispatches after a block of
 * constants, at register `b0`.
 * @param data Required non-null; `size` bytes, copied; a multiple of 4, at
 * most #TB_RHI_PUSH_CONSTANT_BYTES.
 * @par Threading Externally synchronized.
 */
TB_API void tb_rhi_cmd_push_constants(tb_rhi_command_list *list, const void *data, size_t size);

/**
 * @brief Bind a range of a constant buffer at register `b1` plus `slot`.
 * @param slot Below #TB_RHI_CONSTANT_BUFFER_SLOTS.
 * @param offset A multiple of #TB_RHI_CONSTANT_ALIGNMENT.
 * @par Threading Externally synchronized.
 */
TB_API void tb_rhi_cmd_constant_buffer(tb_rhi_command_list *list, uint32_t slot,
                                       tb_rhi_buffer buffer, uint64_t offset);

/** @brief Bind a vertex buffer of the pipeline from an offset.
 * @param slot Below #TB_RHI_VERTEX_BUFFERS.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_vertex_buffer(tb_rhi_command_list *list, uint32_t slot, tb_rhi_buffer buffer,
                                     uint64_t offset);

/** @brief Bind an index buffer of 16- or 32-bit indices.
 * @param format #TB_RHI_FORMAT_R16_UINT or #TB_RHI_FORMAT_R32_UINT.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_index_buffer(tb_rhi_command_list *list, tb_rhi_buffer buffer,
                                    uint64_t offset, tb_rhi_format format);

/** @brief Draw vertices, in a pass.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_draw(tb_rhi_command_list *list, uint32_t vertices, uint32_t instances,
                            uint32_t first_vertex, uint32_t first_instance);

/** @brief Draw indexed vertices, in a pass.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_draw_indexed(tb_rhi_command_list *list, uint32_t indices, uint32_t instances,
                                    uint32_t first_index, int32_t vertex_offset,
                                    uint32_t first_instance);

/** @brief Run a compute pipeline's groups, out of any pass.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_dispatch(tb_rhi_command_list *list, uint32_t x, uint32_t y, uint32_t z);

/** @brief Copy bytes between buffers, out of any pass.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_copy_buffer(tb_rhi_command_list *list, tb_rhi_buffer dest,
                                   uint64_t dest_offset, tb_rhi_buffer source,
                                   uint64_t source_offset, uint64_t size);

/** @brief Copy from a buffer to a texture's image, out of any pass.
 * @param copy Required non-null.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_copy_buffer_to_texture(tb_rhi_command_list *list,
                                              const tb_rhi_texture_copy *copy);

/** @brief Copy from a texture's image to a buffer, out of any pass.
 * @param copy Required non-null.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_copy_texture_to_buffer(tb_rhi_command_list *list,
                                              const tb_rhi_texture_copy *copy);

/** @brief Name the commands after, until tb_rhi_cmd_end_marker(), for
 * debuggers such as PIX and RenderDoc.
 * @param name Required non-null; copied.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_begin_marker(tb_rhi_command_list *list, const char *name);

/** @brief End the innermost marker.
 * @par Threading Externally synchronized. */
TB_API void tb_rhi_cmd_end_marker(tb_rhi_command_list *list);

#endif
