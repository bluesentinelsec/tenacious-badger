/** @file shader.h
 * @brief The shader compiler: shaders written once, in HLSL, for every backend.
 *
 * Shaders are written in HLSL, as Unreal's are, and compiled by the engine
 * for whichever backend runs them: ahead of time by `badger-shaderc` and at
 * run time by the engine, with the same result. See docs/shaders.md.
 *
 * This header reads a shader's sources (tb_shader_preprocess()), and
 * compiles an entry point into its reflection (tb_shader_compile()): what
 * the stage takes, gives and binds, which a pipeline is built from. Writing
 * each backend's output comes next.
 *
 * **Virtual shader paths.** Every shader file is named by an absolute
 * virtual path, as in Unreal: `/Engine/Private/DeferredLighting.hlsl`,
 * `/Engine/Public/Common.hlsli`, `/Game/Water.hlsl`. A `.hlsl` file is a
 * shader with entry points; a `.hlsli` file is included by others. A path
 * has a root (`/Engine`, `/Game`, `/Plugin`) and at least one more
 * component, and is checked by tb_shader_path_check().
 *
 * **Where files come from.** A compile reads its files through a loader.
 * tb_shader_vfs_loader() reads them from a VFS (badger/vfs.h), in which the
 * path `/Engine/Public/Common.hlsli` is the name `Engine/Public/Common.hlsli`:
 * the engine mounts its shaders at `Engine`, a game at `Game`, and a mod
 * mounted over a game overrides its shaders as it overrides its other files.
 * A compile can also supply files of its own in memory, such as the
 * generated `/Engine/Generated/...` files of Unreal; those win over the loader.
 *
 * **The preprocessor** behaves as DXC's does: object-like and function-like
 * macros, variadic macros, `#` and `##`, conditionals with `defined`,
 * `#include`, `#pragma once`, `#error`, `#warning` and `#line`. Other pragmas,
 * such as `#pragma pack_matrix`, are passed on. An include names a virtual
 * path, or a path relative to the including file.
 *
 * **Errors** name the virtual path, the line and the column, in the form
 * Visual Studio and other editors jump to:
 * `/Engine/Private/Foo.hlsl(12,5): error: unterminated comment`.
 *
 * @par Threading
 * tb_shader_preprocess() and tb_shader_compile() are thread-safe when each
 * call has its own arena and the loader is thread-safe, as the VFS loader
 * is. Every other function is thread-safe.
 */
#ifndef TB_SHADER_H
#define TB_SHADER_H

#include "badger/api.h"
#include "badger/arena.h"

#include <stdbool.h>
#include <stddef.h>

struct tb_vfs;

/** @brief Why a compile failed. */
typedef enum tb_shader_result
{
    TB_SHADER_OK,       /**< Success; the output may still hold warnings. */
    TB_SHADER_INVALID,  /**< The request is malformed, such as a main path that is not valid. */
    TB_SHADER_FAILED,   /**< The sources have errors, which the diagnostics describe. */
    TB_SHADER_NO_MEMORY /**< The caller's arena could not hold the output. */
} tb_shader_result;

/** @brief How serious a diagnostic is. */
typedef enum tb_shader_severity
{
    TB_SHADER_ERROR,  /**< The compile fails. */
    TB_SHADER_WARNING /**< The compile goes on. */
} tb_shader_severity;

/** @brief One error or warning, and where it is. */
typedef struct tb_shader_diagnostic
{
    tb_shader_severity severity; /**< How serious it is. */
    /** The virtual path of the file, or "" when the request itself is at fault. */
    const char *path;
    size_t line;         /**< Line, from 1; 0 when there is no file. */
    size_t column;       /**< Byte in the line, from 1; 0 when there is no file. */
    const char *message; /**< What is wrong, in lower case, without a final period. */
} tb_shader_diagnostic;

/** @brief A macro a compile defines before reading its main file, as `/D` does for DXC. */
typedef struct tb_shader_define
{
    const char *name;  /**< Required; an identifier. */
    const char *value; /**< Its replacement as text, or NULL for `1`. */
} tb_shader_define;

/** @brief A file a compile supplies in memory, which wins over the loader's. */
typedef struct tb_shader_file
{
    const char *path; /**< Required; a virtual path, NUL-terminated. */
    const char *text; /**< The file; NULL only at length zero. Borrowed for the call. */
    size_t length;    /**< Bytes of `text`. */
} tb_shader_file;

/** @brief What a loader found. */
typedef enum tb_shader_load_result
{
    TB_SHADER_LOAD_OK,        /**< The file was read. */
    TB_SHADER_LOAD_NOT_FOUND, /**< There is no such file. */
    TB_SHADER_LOAD_FAILED     /**< The file exists, but could not be read. */
} tb_shader_load_result;

/**
 * @brief Read a shader file whole.
 * @param context The loader's context.
 * @param path A virtual path, already checked, NUL-terminated.
 * @param length Bytes of `path`.
 * @param arena Where to put the text; it lasts as long as the compile needs it.
 * @param[out] text The file's bytes, in `arena`, on OK.
 * @param[out] text_length Bytes of `text`, on OK.
 * @return What was found.
 */
typedef tb_shader_load_result tb_shader_load_function(void *context, const char *path,
                                                      size_t length, tb_arena *arena,
                                                      const char **text, size_t *text_length);

/** @brief Where a compile reads its files from. */
typedef struct tb_shader_loader
{
    tb_shader_load_function *load; /**< Required. */
    void *context;                 /**< Passed to `load`. */
} tb_shader_loader;

/** @brief The preprocessed text marks each change of file and each jump in lines with `#line`. */
#define TB_SHADER_LINE_MARKERS 1u

/** @brief What to preprocess. Zero-initialize, then fill in what is needed. */
typedef struct tb_shader_preprocess_request
{
    const char *path;                /**< Required; the main file's virtual path, NUL-terminated. */
    const tb_shader_define *defines; /**< Macros defined first; NULL only at count zero. */
    size_t define_count;             /**< Entries of `defines`. */
    const tb_shader_file *files;     /**< Files in memory; NULL only at count zero. */
    size_t file_count;               /**< Entries of `files`. */
    /** Where every other file comes from, or NULL when `files` holds them all. Borrowed. */
    const tb_shader_loader *loader;
    unsigned flags; /**< A combination of TB_SHADER_LINE_MARKERS. */
    /** Tokens the preprocessor may make, from files and macros alike, which
     * bounds what a source can expand to; 0 for TB_SHADER_MAX_TOKENS. */
    size_t max_tokens;
} tb_shader_preprocess_request;

/** @brief A preprocessed shader. Everything in it lives in the caller's arena. */
typedef struct tb_shader_preprocessed
{
    const char *text; /**< The preprocessed text, with a NUL after it; "" on failure. */
    size_t length;    /**< Bytes of `text` before the NUL. */
    /** Every file the compile read, main file first, each once, in the order it was first read:
     * what the shader cache keys on, and what hot reload watches. */
    const char *const *dependencies;
    size_t dependency_count;                 /**< Entries of `dependencies`. */
    const tb_shader_diagnostic *diagnostics; /**< Errors and warnings, in the order found. */
    size_t diagnostic_count;                 /**< Entries of `diagnostics`. */
} tb_shader_preprocessed;

/** @brief Files included inside one another, the main file counting as one. */
#define TB_SHADER_MAX_INCLUDE_DEPTH ((size_t)64)

/** @brief Bytes of one file. */
#define TB_SHADER_MAX_FILE_SIZE ((size_t)16 << 20)

/** @brief Tokens the preprocessor makes for one shader when the request gives no limit. */
#define TB_SHADER_MAX_TOKENS ((size_t)1 << 20)

/** @brief Errors reported before a compile stops looking for more. */
#define TB_SHADER_MAX_ERRORS ((size_t)64)

/**
 * @brief Check a virtual shader path.
 *
 * A valid path starts with `/`, has a root and at least one more component,
 * and ends in `.hlsl` or `.hlsli`. No component is empty, `.` or `..`, and
 * no byte is a `\`, a `:`, a control character or a space, so a path means
 * the same thing on every system.
 * @param path NULL only at length zero. @param length Bytes of `path`.
 * @return Whether the path is valid.
 */
TB_API bool tb_shader_path_check(const char *path, size_t length);

/**
 * @brief Preprocess a shader: read its main file and everything it includes,
 * and apply its directives and macros.
 * @param request Required.
 * @param arena Where the output lives, until the arena is reset or destroyed.
 * @param[out] output Required. On OK and FAILED, filled in: FAILED has the
 * errors. On INVALID, the diagnostics say why. On NO_MEMORY, zero.
 * @return OK, INVALID, FAILED or NO_MEMORY.
 * @par Errors Running out of memory for the compile's own work ends the
 * process, as allocation does. Only the output's arena reports NO_MEMORY.
 */
TB_API tb_shader_result tb_shader_preprocess(const tb_shader_preprocess_request *request,
                                             tb_arena *arena, tb_shader_preprocessed *output);

/**
 * @brief A loader that reads shader files from a VFS, the path `/Engine/X.hlsli`
 * as the name `Engine/X.hlsli`.
 * @param vfs Required; borrowed, and must outlive every compile that uses the loader.
 * @return The loader. A file over TB_SHADER_MAX_FILE_SIZE is FAILED.
 */
TB_API tb_shader_loader tb_shader_vfs_loader(struct tb_vfs *vfs);

/**
 * @brief Write a diagnostic as editors read it: `path(line,column): error: message`,
 * or `error: message` when it has no file.
 * @param diagnostic Required.
 * @param buffer Where to write; NULL only at size zero. Always NUL-terminated when size is not
 * zero.
 * @param size Bytes of `buffer`.
 * @return Bytes the whole text needs, without the NUL, as tb_fmt_buffer() returns.
 */
TB_API size_t tb_shader_diagnostic_format(const tb_shader_diagnostic *diagnostic, char *buffer,
                                          size_t size);

/* -------------------------------------------------------------------------
 * Compiling and reflection.
 * ------------------------------------------------------------------------- */

/** @brief The stages a shader runs in, as `[shader("...")]` names them. */
typedef enum tb_shader_stage
{
    TB_SHADER_STAGE_VERTEX,         /**< "vertex" */
    TB_SHADER_STAGE_PIXEL,          /**< "pixel" */
    TB_SHADER_STAGE_GEOMETRY,       /**< "geometry" */
    TB_SHADER_STAGE_HULL,           /**< "hull" */
    TB_SHADER_STAGE_DOMAIN,         /**< "domain" */
    TB_SHADER_STAGE_COMPUTE,        /**< "compute" */
    TB_SHADER_STAGE_AMPLIFICATION,  /**< "amplification" */
    TB_SHADER_STAGE_MESH,           /**< "mesh" */
    TB_SHADER_STAGE_RAY_GENERATION, /**< "raygeneration" */
    TB_SHADER_STAGE_INTERSECTION,   /**< "intersection" */
    TB_SHADER_STAGE_ANY_HIT,        /**< "anyhit" */
    TB_SHADER_STAGE_CLOSEST_HIT,    /**< "closesthit" */
    TB_SHADER_STAGE_MISS,           /**< "miss" */
    TB_SHADER_STAGE_CALLABLE        /**< "callable" */
} tb_shader_stage;

/** @brief The component of a value a stage passes in or out, or a resource holds. */
typedef enum tb_shader_component
{
    TB_SHADER_COMPONENT_FLOAT32,
    TB_SHADER_COMPONENT_INT32,
    TB_SHADER_COMPONENT_UINT32, /**< A bool's too. */
    TB_SHADER_COMPONENT_FLOAT16,
    TB_SHADER_COMPONENT_INT16,
    TB_SHADER_COMPONENT_UINT16,
    TB_SHADER_COMPONENT_FLOAT64,
    TB_SHADER_COMPONENT_INT64,
    TB_SHADER_COMPONENT_UINT64,
    TB_SHADER_COMPONENT_MIN16_FLOAT, /**< `min16float`: 16 bits or more. */
    TB_SHADER_COMPONENT_MIN16_INT,
    TB_SHADER_COMPONENT_MIN16_UINT
} tb_shader_component;

/** @brief How a value a pixel shader takes in is interpolated across a primitive. */
typedef enum tb_shader_interpolation
{
    TB_SHADER_INTERPOLATION_NONE,     /**< Not interpolated: an output, or a stage that does not. */
    TB_SHADER_INTERPOLATION_LINEAR,   /**< Perspective-correct; a float's default. */
    TB_SHADER_INTERPOLATION_CONSTANT, /**< `nointerpolation`; an integer's and a bool's. */
    TB_SHADER_INTERPOLATION_NOPERSPECTIVE,
    TB_SHADER_INTERPOLATION_CENTROID,
    TB_SHADER_INTERPOLATION_NOPERSPECTIVE_CENTROID,
    TB_SHADER_INTERPOLATION_SAMPLE,
    TB_SHADER_INTERPOLATION_NOPERSPECTIVE_SAMPLE
} tb_shader_interpolation;

/**
 * @brief One value a stage passes in or out: a semantic and its index.
 *
 * A struct passes one element for each member, and an array or a matrix one
 * for each element or row, its semantic's index counting up from the one
 * written: `float2 uv[2] : TEXCOORD3` is TEXCOORD 3 and TEXCOORD 4.
 */
typedef struct tb_shader_signature_element
{
    const char *semantic;    /**< Its name without the index: `TEXCOORD`, `SV_Position`. */
    unsigned semantic_index; /**< 0 when the name has no digits after it. */
    tb_shader_component component;
    unsigned columns; /**< Components, from 1 to 4. */
    tb_shader_interpolation interpolation;
} tb_shader_signature_element;

/** @brief What a binding binds, which decides its register class. */
typedef enum tb_shader_binding_kind
{
    TB_SHADER_BINDING_CONSTANT_BUFFER, /**< `b`: a cbuffer, a ConstantBuffer<S>, `$Globals`. */
    TB_SHADER_BINDING_TEXTURE_BUFFER,  /**< `t`: a tbuffer or a TextureBuffer<S>. */
    TB_SHADER_BINDING_READ,            /**< `t`: a texture or a buffer that is read. */
    TB_SHADER_BINDING_READ_WRITE,      /**< `u`: one that is written too. */
    TB_SHADER_BINDING_SAMPLER          /**< `s` */
} tb_shader_binding_kind;

/** @brief The shape of a texture or a buffer a binding binds. */
typedef enum tb_shader_binding_shape
{
    TB_SHADER_SHAPE_NONE, /**< A constant buffer or a sampler. */
    TB_SHADER_SHAPE_BUFFER,
    TB_SHADER_SHAPE_STRUCTURED_BUFFER,
    TB_SHADER_SHAPE_BYTE_ADDRESS_BUFFER,
    TB_SHADER_SHAPE_TEXTURE1D,
    TB_SHADER_SHAPE_TEXTURE1D_ARRAY,
    TB_SHADER_SHAPE_TEXTURE2D,
    TB_SHADER_SHAPE_TEXTURE2D_ARRAY,
    TB_SHADER_SHAPE_TEXTURE2DMS,
    TB_SHADER_SHAPE_TEXTURE2DMS_ARRAY,
    TB_SHADER_SHAPE_TEXTURE3D,
    TB_SHADER_SHAPE_TEXTURE_CUBE,
    TB_SHADER_SHAPE_TEXTURE_CUBE_ARRAY,
    TB_SHADER_SHAPE_ACCELERATION_STRUCTURE,
    TB_SHADER_SHAPE_FEEDBACK_TEXTURE2D,
    TB_SHADER_SHAPE_FEEDBACK_TEXTURE2D_ARRAY
} tb_shader_binding_shape;

/**
 * @brief A constant buffer, texture, buffer or sampler a stage binds, and
 * where: in a register class (by its kind), a register and a space.
 *
 * Only what the entry point uses is bound, as in DXC. A binding the source
 * gives no register takes the lowest registers of its class in space 0 that
 * are free, in the order of the source, `$Globals` first, as DXC assigns them.
 */
typedef struct tb_shader_binding
{
    const char *name;
    tb_shader_binding_kind kind;
    tb_shader_binding_shape shape;
    unsigned space;
    unsigned register_index; /**< The first register. */
    unsigned count; /**< Registers: an array's elements, 1 alone; 0 for an array of no size. */
    /** A texture's or a typed buffer's element: its component and components; 0 columns for none.
     */
    tb_shader_component element;
    unsigned element_columns;
} tb_shader_binding;

/** @brief What a constant buffer's variable is. */
typedef enum tb_shader_variable_class
{
    TB_SHADER_VARIABLE_SCALAR,
    TB_SHADER_VARIABLE_VECTOR,
    TB_SHADER_VARIABLE_MATRIX_ROWS,    /**< A `row_major` matrix: a register for each row. */
    TB_SHADER_VARIABLE_MATRIX_COLUMNS, /**< A `column_major` matrix, as HLSL's are by default. */
    TB_SHADER_VARIABLE_STRUCT
} tb_shader_variable_class;

/**
 * @brief A variable of a constant buffer, or a member of one of its structs:
 * where it is, and what it is.
 *
 * Offsets are bytes from the buffer's start, laid out as HLSL lays out a
 * constant buffer: in registers of 16 bytes that no vector crosses; each
 * array element, matrix and struct starting a register. An array's or a
 * struct's members are described for its first element.
 */
typedef struct tb_shader_variable
{
    const char *name;
    unsigned offset; /**< From the buffer's start. */
    unsigned size;   /**< Bytes from its start to its last value's end. */
    tb_shader_variable_class variable_class;
    tb_shader_component component; /**< Not a struct's. */
    unsigned rows;                 /**< 1 but for a matrix. */
    unsigned columns;              /**< A vector's or a matrix's components in a row. */
    unsigned elements;             /**< An array's; 0 when it is none. */
    unsigned stride;               /**< An array's: bytes from one element to the next. */
    /** A struct's members: these entries of its buffer's `variables`. */
    unsigned first_member;
    unsigned member_count;
} tb_shader_variable;

/** @brief A constant buffer's layout. */
typedef struct tb_shader_constant_buffer
{
    const char *name; /**< As its binding's. */
    unsigned size;    /**< Bytes from its start to its last variable's end. */
    /** Its variables, then the members of its structs: the first `top_count`
     * are its own, in order; a struct's are where `first_member` says. */
    const tb_shader_variable *variables;
    size_t variable_count;
    size_t top_count;
} tb_shader_constant_buffer;

/** @brief What a stage takes, gives and binds: what a pipeline is built from. */
typedef struct tb_shader_reflection
{
    tb_shader_stage stage;
    /** `[numthreads(x, y, z)]` of a compute, mesh or amplification shader; zeros otherwise. */
    unsigned thread_group[3];
    const tb_shader_signature_element *inputs; /**< In the order of the entry point's parameters. */
    size_t input_count;
    const tb_shader_signature_element *outputs; /**< Its result's, then its `out` parameters'. */
    size_t output_count;
    const tb_shader_binding *bindings; /**< By kind (`b`, `s`, `t`, `u`), then as assigned. */
    size_t binding_count;
    /** The layout of each constant and texture buffer bound, in the order of `bindings`. */
    const tb_shader_constant_buffer *buffers;
    size_t buffer_count;
} tb_shader_reflection;

/** @brief What to compile: a source, and the function a stage runs from it. */
typedef struct tb_shader_compile_request
{
    tb_shader_preprocess_request source; /**< What to read, as tb_shader_preprocess() reads it. */
    const char *entry_point;             /**< Required; the function's name, NUL-terminated. */
    tb_shader_stage stage;
} tb_shader_compile_request;

/** @brief A compiled shader. Everything in it lives in the caller's arena. */
typedef struct tb_shader_compiled
{
    tb_shader_reflection reflection;         /**< Filled in on OK. */
    const tb_shader_diagnostic *diagnostics; /**< Errors and warnings, in the order found. */
    size_t diagnostic_count;                 /**< Entries of `diagnostics`. */
} tb_shader_compiled;

/**
 * @brief Compile a shader's entry point for a stage, and describe what it
 * takes, gives and binds.
 *
 * Reads the source as tb_shader_preprocess() does, checks it as DXC does,
 * and lowers it into the compiler's own form, of which the entry point keeps
 * only what it reaches. Writing each backend's output comes later; for now
 * the result is the reflection.
 * @param request Required.
 * @param arena Where the output lives, until the arena is reset or destroyed.
 * @param[out] output Required. On OK, filled in; on FAILED and INVALID, the
 * diagnostics say why; on NO_MEMORY, zero.
 * @return OK, INVALID, FAILED or NO_MEMORY.
 * @par Errors Running out of memory for the compile's own work ends the
 * process, as allocation does. Only the output's arena reports NO_MEMORY.
 */
TB_API tb_shader_result tb_shader_compile(const tb_shader_compile_request *request, tb_arena *arena,
                                          tb_shader_compiled *output);

/**
 * @brief Name a stage as `[shader("...")]` does: "pixel", "closesthit".
 * @param stage Any value; one that is no stage is named "unknown stage".
 * @return A string with static storage duration; never NULL.
 */
TB_API const char *tb_shader_stage_name(tb_shader_stage stage);

/**
 * @brief Name a result, for a log or a message: "failed", "no memory".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 */
TB_API const char *tb_shader_result_name(tb_shader_result result);

#endif
