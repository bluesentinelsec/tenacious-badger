/* badger-shaderc: compiles the shaders a manifest lists, ahead of time and in
 * parallel, into a shader library for each backend asked for, as Unreal's
 * ShaderCompileWorker compiles for its cooker. It compiles through the
 * engine's own shader module, and shares the engine's shader cache, so what
 * it writes is what the engine would compile at run time, byte for byte.
 * See docs/shader-cache.md. */
#include "badger/arena.h"
#include "badger/cli.h"
#include "badger/crash.h"
#include "badger/d3dcompiler.h"
#include "badger/dxc.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/jobs.h"
#include "badger/memory.h"
#include "badger/path.h"
#include "badger/process.h"
#include "badger/shader.h"
#include "badger/shader_cache.h"
#include "badger/shader_library.h"
#include "badger/str.h"
#include "badger/version.h"
#include "badger/vfs.h"

#include <stdlib.h>
#include <string.h>

#define SHADERC_MOST_ROOTS ((size_t)16)
#define SHADERC_MOST_BACKENDS ((size_t)8)
#define SHADERC_PATH_CAPACITY ((size_t)4096)
/* Bytes of a manifest at most. */
#define SHADERC_MOST_MANIFEST ((size_t)16 << 20)

static const char *shaderc_manifest_path = NULL;
static const char *shaderc_roots[SHADERC_MOST_ROOTS] = {0};
static size_t shaderc_root_count = 0;
static const char *shaderc_backends[SHADERC_MOST_BACKENDS] = {0};
static size_t shaderc_backend_count = 0;
static const char *shaderc_output = ".";
static const char *shaderc_cache_path = NULL;
static bool shaderc_no_cache = false;
static bool shaderc_debug = false;
static bool shaderc_quiet = false;

/* A backend a library is written for, and what its shaders compile into. */
typedef struct shaderc_backend
{
    const char *name;    /* As `graphics.renderer` names it. */
    const char *library; /* Its library's file name. */
    tb_shader_target target;
} shaderc_backend;

static const shaderc_backend shaderc_known_backends[] = {
    {"d3d12", "d3d12.tbsl", TB_SHADER_TARGET_DXIL},
    {"d3d11", "d3d11.tbsl", TB_SHADER_TARGET_DXBC},
};

/* One shader of the manifest, compiled for one backend. */
typedef struct shaderc_shader
{
    const tb_shader_manifest_entry *entry;
    tb_arena *arena; /* What the compile gave lives here. */
    tb_shader_result result;
    tb_shader_compiled compiled;
    tb_shader_key key;
    tb_shader_origin origin;
} shaderc_shader;

/* What every job of a backend shares; only read while jobs run. */
typedef struct shaderc_work
{
    const shaderc_backend *backend;
    tb_shader_loader loader;
    tb_dxc *dxc;
    tb_d3dcompiler *d3dcompiler;
    tb_shader_cache *cache;
    shaderc_shader *shaders;
} shaderc_work;

static void shaderc_compile_range(void *argument, size_t first, size_t count)
{
    shaderc_work *const work = argument;
    for (size_t index = first; index < first + count; index++)
    {
        shaderc_shader *const shader = &work->shaders[index];
        const tb_shader_manifest_entry *const entry = shader->entry;
        tb_shader_compile_request request = {0};
        request.source.path = entry->path;
        request.source.defines = entry->defines;
        request.source.define_count = entry->define_count;
        request.source.loader = &work->loader;
        request.entry_point = entry->entry_point;
        request.stage = entry->stage;
        request.target = work->backend->target;
        request.flags = shaderc_debug ? TB_SHADER_COMPILE_DEBUG : 0u;
        request.dxc = work->dxc;
        request.d3dcompiler = work->d3dcompiler;
        const tb_shader_lookup lookup = {.cache = work->cache};
        shader->arena = tb_arena_create(0);
        shader->result = tb_shader_compile_cached(&request, &lookup, shader->arena,
                                                  &shader->compiled, &shader->key, &shader->origin);
    }
}

/* Write a shader's errors and warnings, each as editors read it. Returns
 * whether it compiled. */
static bool shaderc_report(const shaderc_shader *shader, const char *manifest)
{
    for (size_t index = 0; index < shader->compiled.diagnostic_count; index++)
    {
        char line[1024] = {0};
        (void)tb_shader_diagnostic_format(&shader->compiled.diagnostics[index], line, sizeof(line));
        (void)tb_fmt_print_error("%s\n", line);
    }
    if (shader->result == TB_SHADER_OK)
    {
        return true;
    }
    (void)tb_fmt_print_error("%s(%zu,1): error: %s %s of %s did not compile: %s\n", manifest,
                             shader->entry->line, tb_shader_stage_name(shader->entry->stage),
                             shader->entry->entry_point, shader->entry->path,
                             tb_shader_result_name(shader->result));
    return false;
}

static bool shaderc_write_library(const shaderc_work *work, size_t count, const char *compiler)
{
    tb_shader_library_writer *const writer = tb_shader_library_writer_create();
    tb_arena *const arena = tb_arena_create(0);
    char *path = NULL;
    tb_path_result joined = TB_PATH_OK;
    const void *bytes = NULL;
    size_t size = 0;
    bool written = false;
    size_t cached = 0;
    for (size_t index = 0; index < count; index++)
    {
        const shaderc_shader *const shader = &work->shaders[index];
        (void)tb_shader_library_writer_add(writer, &shader->key, shader->compiled.code,
                                           shader->compiled.code_size);
        cached += shader->origin == TB_SHADER_ORIGIN_CACHE ? 1u : 0u;
    }
    const tb_path_view output = {shaderc_output, tb_str_length(shaderc_output)};
    const tb_path_view name = {work->backend->library, tb_str_length(work->backend->library)};
    path = tb_path_join_new(NULL, tb_fs_native_style(), output, name, &joined);
    if (path == NULL || tb_fs_make_directories(output) != TB_FS_OK)
    {
        (void)tb_fmt_print_error("badger-shaderc: error: cannot make the directory %s\n",
                                 shaderc_output);
        goto cleanup;
    }
    if (!tb_shader_library_writer_finish(writer, compiler, arena, &bytes, &size))
    {
        (void)tb_fmt_print_error("badger-shaderc: error: %s's library is too large\n",
                                 work->backend->name);
        goto cleanup;
    }
    const tb_path_view target = {path, tb_str_length(path)};
    if (tb_fs_replace_file(target, bytes, size) != TB_FS_OK)
    {
        (void)tb_fmt_print_error("badger-shaderc: error: cannot write %s\n", path);
        goto cleanup;
    }
    written = true;
    if (!shaderc_quiet)
    {
        (void)tb_fmt_print("badger-shaderc: %s: %zu shaders, %zu of them from the cache, into %s\n",
                           work->backend->name, count, cached, path);
    }
cleanup:
    tb_str_free(NULL, path);
    tb_arena_destroy(arena);
    tb_shader_library_writer_destroy(writer);
    return written;
}

/* Compile every shader of the manifest for a backend, then write its library. */
static bool shaderc_build(const shaderc_backend *backend, const tb_shader_manifest *manifest,
                          tb_vfs *vfs, tb_shader_cache *cache, tb_jobs *jobs)
{
    tb_dxc *dxc = NULL;
    tb_dxc_error error = TB_DXC_ERROR_INIT;
    tb_d3dcompiler *d3dcompiler = NULL;
    tb_d3dcompiler_error d3dcompiler_error = TB_D3DCOMPILER_ERROR_INIT;
    const char *compiler = NULL;
    bool built = true;
    if (backend->target == TB_SHADER_TARGET_DXIL && tb_dxc_open(&dxc, &error) != TB_DXC_OK)
    {
        (void)tb_fmt_print_error("badger-shaderc: error: %s needs DXC: %s\n", backend->name,
                                 error.message);
        return false;
    }
    if (backend->target == TB_SHADER_TARGET_DXBC &&
        tb_d3dcompiler_open(&d3dcompiler, &d3dcompiler_error) != TB_D3DCOMPILER_OK)
    {
        (void)tb_fmt_print_error("badger-shaderc: error: %s needs D3DCompiler: %s\n", backend->name,
                                 d3dcompiler_error.message);
        return false;
    }
    compiler = dxc != NULL ? tb_dxc_version(dxc) : tb_d3dcompiler_version(d3dcompiler);
    shaderc_work work = {0};
    work.backend = backend;
    work.loader = tb_shader_vfs_loader(vfs);
    work.dxc = dxc;
    work.d3dcompiler = d3dcompiler;
    work.cache = cache;
    work.shaders =
        tb_calloc(manifest->entry_count == 0 ? 1 : manifest->entry_count, sizeof(shaderc_shader));
    for (size_t index = 0; index < manifest->entry_count; index++)
    {
        work.shaders[index].entry = &manifest->entries[index];
    }
    tb_jobs_for(jobs, manifest->entry_count, 1, shaderc_compile_range, &work);
    for (size_t index = 0; index < manifest->entry_count; index++)
    {
        built = shaderc_report(&work.shaders[index], shaderc_manifest_path) && built;
    }
    if (built)
    {
        built = shaderc_write_library(&work, manifest->entry_count, compiler);
    }
    for (size_t index = 0; index < manifest->entry_count; index++)
    {
        tb_arena_destroy(work.shaders[index].arena);
    }
    tb_free(work.shaders);
    tb_d3dcompiler_close(d3dcompiler);
    tb_dxc_close(dxc);
    return built;
}

/* The backend a name names; NULL, reported, for one with no compiler yet. */
static const shaderc_backend *shaderc_find_backend(const char *name)
{
    for (size_t index = 0; index < TB_CLI_COUNT_OF(shaderc_known_backends); index++)
    {
        if (tb_str_equal(shaderc_known_backends[index].name, name))
        {
            return &shaderc_known_backends[index];
        }
    }
    (void)tb_fmt_print_error("badger-shaderc: error: no shader library is written for '%s' yet; "
                             "the backends with libraries are: d3d12, d3d11\n",
                             name);
    return NULL;
}

/* Read the manifest; false, with its errors written, when it cannot be. */
static bool shaderc_read_manifest(tb_arena *arena, tb_shader_manifest *manifest)
{
    tb_fs_buffer text = {0};
    const tb_path_view path = {shaderc_manifest_path, tb_str_length(shaderc_manifest_path)};
    const tb_fs_result read = tb_fs_read_file(path, NULL, SHADERC_MOST_MANIFEST, &text);
    if (read != TB_FS_OK)
    {
        (void)tb_fmt_print_error("badger-shaderc: error: cannot read %s: %s\n",
                                 shaderc_manifest_path, tb_fs_result_name(read));
        return false;
    }
    const tb_shader_result result = tb_shader_manifest_read((const char *)text.data, text.length,
                                                            shaderc_manifest_path, arena, manifest);
    tb_fs_buffer_free(&text);
    for (size_t index = 0; index < manifest->diagnostic_count; index++)
    {
        char line[1024] = {0};
        (void)tb_shader_diagnostic_format(&manifest->diagnostics[index], line, sizeof(line));
        (void)tb_fmt_print_error("%s\n", line);
    }
    return result == TB_SHADER_OK;
}

/* Mount each root at the top of the VFS, later roots over earlier ones. */
static bool shaderc_mount_roots(tb_vfs *vfs)
{
    for (size_t index = 0; index < shaderc_root_count; index++)
    {
        const tb_vfs_result mounted =
            tb_vfs_mount_directory(vfs, "", 0, shaderc_roots[index], (int)index);
        if (mounted != TB_VFS_OK)
        {
            (void)tb_fmt_print_error("badger-shaderc: error: cannot read shaders from %s: %s; "
                                     "give an absolute path to a directory\n",
                                     shaderc_roots[index], tb_vfs_result_name(mounted));
            return false;
        }
    }
    return true;
}

/* Open the shader cache: the one given, or the user's own, which the engine
 * shares. A cache that cannot be opened is no error: compiles go on without. */
static tb_shader_cache *shaderc_open_cache(void)
{
    char path[SHADERC_PATH_CAPACITY] = {0};
    tb_shader_cache *cache = NULL;
    if (shaderc_no_cache)
    {
        return NULL;
    }
    const char *directory = shaderc_cache_path;
    if (directory == NULL)
    {
        if (!tb_shader_cache_default_directory(path, sizeof(path)))
        {
            (void)tb_fmt_print_error("badger-shaderc: warning: this system has no cache "
                                     "directory; compiling without the shader cache\n");
            return NULL;
        }
        directory = path;
    }
    if (tb_shader_cache_open(directory, &cache) != TB_SHADER_CACHE_OK)
    {
        (void)tb_fmt_print_error("badger-shaderc: warning: cannot open the shader cache %s; "
                                 "compiling without it\n",
                                 directory);
    }
    return cache;
}

static int shaderc_main(const tb_cli_outcome *outcome, void *context)
{
    tb_arena *const arena = tb_arena_create(0);
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_shader_manifest manifest = {0};
    tb_shader_cache *cache = NULL;
    tb_jobs *jobs = NULL;
    int status = EXIT_FAILURE;
    (void)outcome;
    (void)context;
    if (!shaderc_read_manifest(arena, &manifest) || !shaderc_mount_roots(vfs))
    {
        goto cleanup;
    }
    for (size_t index = 0; index < shaderc_backend_count; index++)
    {
        if (shaderc_find_backend(shaderc_backends[index]) == NULL)
        {
            goto cleanup;
        }
    }
    if (shaderc_backend_count == 0)
    {
        (void)tb_fmt_print("badger-shaderc: no backend given; nothing to compile\n");
        status = EXIT_SUCCESS;
        goto cleanup;
    }
    cache = shaderc_open_cache();
    jobs = tb_jobs_create(NULL);
    status = EXIT_SUCCESS;
    for (size_t index = 0; index < shaderc_backend_count; index++)
    {
        const shaderc_backend *const backend = shaderc_find_backend(shaderc_backends[index]);
        if (!shaderc_build(backend, &manifest, vfs, cache, jobs))
        {
            status = EXIT_FAILURE;
        }
    }
cleanup:
    tb_jobs_destroy(jobs);
    tb_shader_cache_close(cache);
    tb_vfs_destroy(vfs);
    tb_arena_destroy(arena);
    return status;
}

static const tb_cli_option shaderc_options[] = {
    {.name = "MANIFEST",
     .text = &shaderc_manifest_path,
     .required = true,
     .help = "The manifest: the shaders to compile, one to a line."},
    {.long_name = "root",
     .name = "DIRECTORY",
     .texts = shaderc_roots,
     .capacity = SHADERC_MOST_ROOTS,
     .count = &shaderc_root_count,
     .help = "An absolute directory whose files are shaders by their virtual paths: "
             "DIRECTORY/Engine/Private/Sky.hlsl is /Engine/Private/Sky.hlsl. May be given more "
             "than once; a later root's file wins."},
    {.long_name = "backend",
     .name = "NAME",
     .texts = shaderc_backends,
     .capacity = SHADERC_MOST_BACKENDS,
     .count = &shaderc_backend_count,
     .help =
         "A backend to write a library for, as graphics.renderer names it: d3d12 or d3d11. May be "
         "given more than once; none compiles nothing."},
    {.long_name = "output",
     .name = "DIRECTORY",
     .text = &shaderc_output,
     .help = "Where the libraries are written, each named after its backend: d3d12.tbsl. The "
             "default is the current directory."},
    {.long_name = "cache",
     .name = "DIRECTORY",
     .text = &shaderc_cache_path,
     .help = "The shader cache. The default is the user's, which the engine shares."},
    {.long_name = "no-cache", .flag = &shaderc_no_cache, .help = "Compile without the cache."},
    {.long_name = "debug",
     .flag = &shaderc_debug,
     .help = "Compile for debugging: unoptimized, with the debug information PIX reads."},
    {.short_name = 'q',
     .long_name = "quiet",
     .flag = &shaderc_quiet,
     .help = "Say only what fails."},
};

static const tb_cli_command shaderc_program = {
    .name = "badger-shaderc",
    .version = TB_VERSION_STRING,
    .description = "Compile the shaders a manifest lists, ahead of time and in parallel, into a "
                   "shader library for each backend. The engine finds them there by the same "
                   "keys it compiles them by at run time, and both share the shader cache.\n\n",
    .epilog = "A manifest lists a shader on each line: its virtual path, its stage, its entry "
              "point, and any defines, as NAME or NAME=VALUE. A # begins a comment. See "
              "docs/shader-cache.md.\n\n",
    .options = shaderc_options,
    .option_count = TB_CLI_COUNT_OF(shaderc_options),
    .run = shaderc_main,
};

int main(int count, char **values)
{
    tb_arguments arguments = TB_ARGUMENTS_INIT;
    int status = EXIT_FAILURE;

    tb_crash_catch();
    if (!tb_process_get_arguments(count, values, &arguments))
    {
        (void)tb_fmt_print_error("badger-shaderc: the command line could not be read\n");
        return EXIT_FAILURE;
    }
    status = tb_cli_run(&shaderc_program, arguments.count, arguments.values, NULL);
    tb_process_release_arguments(&arguments);
    return status;
}
