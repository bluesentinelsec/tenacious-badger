/* D3DCompiler on Windows: D3DCompiler_47.dll, part of Windows, loaded at run
 * time from the system directory and called through its C entry point. */
#include "badger/d3dcompiler.h"

#include "badger/fmt.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/str.h"

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>

#include <d3dcommon.h>

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* What d3dcompiler.h declares of the library, declared here: the header
 * cannot be compiled as C with every warning on. */
#define D3DCOMPILE_DEBUG (1u << 0)
#define D3DCOMPILE_SKIP_OPTIMIZATION (1u << 2)
#define D3DCOMPILE_ENABLE_STRICTNESS (1u << 11)
#define D3DCOMPILE_OPTIMIZATION_LEVEL3 (1u << 15)
typedef HRESULT(WINAPI *pD3DCompile)(LPCVOID source, SIZE_T size, LPCSTR name,
                                     const D3D_SHADER_MACRO *defines, ID3DInclude *include,
                                     LPCSTR entry_point, LPCSTR target, UINT flags,
                                     UINT effect_flags, ID3DBlob **code, ID3DBlob **messages);

/* The library, as the system names it. */
#define LIBRARY_NAME "d3dcompiler_47.dll"

/* The name the version gives the library. */
#define COMPILER_NAME "d3dcompiler_47"

/* A version resource: a VS_VERSIONINFO, whose fixed part starts after its
 * three counts and its key, "VS_VERSION_INFO" in UTF-16 with a NUL, padded
 * to four bytes; and the signature that fixed part starts with. */
#define FIXED_INFO_OFFSET ((size_t)40)
#define FIXED_INFO_SIGNATURE 0xFEEF04BDu

/* The version resource's identifier and type, as winuser.h has them. */
#define VERSION_RESOURCE 1
#define VERSION_TYPE 16

struct tb_d3dcompiler
{
    tb_library *library;
    pD3DCompile compile;
    char version[TB_D3DCOMPILER_VERSION_CAPACITY];
};

struct tb_d3dcompiler_compiled
{
    ID3DBlob *code;
    ID3DBlob *messages;
};

static tb_d3dcompiler_result fail(tb_d3dcompiler_error *error, tb_d3dcompiler_result result,
                                  const char *format, ...)
{
    if (error != NULL)
    {
        va_list arguments;
        va_start(arguments, format);
        error->result = result;
        (void)tb_fmt_vbuffer(error->message, sizeof(error->message), format, arguments);
        va_end(arguments);
    }
    return result;
}

static void succeed(tb_d3dcompiler_error *error)
{
    if (error != NULL)
    {
        error->result = TB_D3DCOMPILER_OK;
        error->message[0] = '\0';
    }
}

/* -------------------------------------------------------------------------
 * Naming the compiler.
 * ------------------------------------------------------------------------- */

/* Name the library that holds `function` by its file version, into
 * `version`: the module is found from the function's address, and its
 * version resource read in place, so nothing more is loaded. */
static void describe(tb_d3dcompiler *compiler, const void *function)
{
    HMODULE module = NULL;
    HRSRC found = NULL;
    HGLOBAL loaded = NULL;
    const uint8_t *bytes = NULL;
    DWORD size = 0;
    VS_FIXEDFILEINFO fixed = {0};
    (void)tb_fmt_buffer(compiler->version, sizeof(compiler->version), COMPILER_NAME " unknown");
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)function, &module))
    {
        /* coverage-off: the function was just found in a loaded module */
        return;
        /* coverage-on */
    }
    found =
        FindResourceW(module, MAKEINTRESOURCEW(VERSION_RESOURCE), MAKEINTRESOURCEW(VERSION_TYPE));
    loaded = found == NULL ? NULL : LoadResource(module, found);
    bytes = loaded == NULL ? NULL : LockResource(loaded);
    size = found == NULL ? 0 : SizeofResource(module, found);
    if (bytes == NULL || size < FIXED_INFO_OFFSET + sizeof(fixed))
    {
        /* coverage-off: Windows's own D3DCompiler has its version */
        return;
        /* coverage-on */
    }
    memcpy(&fixed, bytes + FIXED_INFO_OFFSET, sizeof(fixed));
    if (fixed.dwSignature != FIXED_INFO_SIGNATURE)
    {
        /* coverage-off: Windows's own D3DCompiler has its version */
        return;
        /* coverage-on */
    }
    (void)tb_fmt_buffer(
        compiler->version, sizeof(compiler->version), COMPILER_NAME " %u.%u.%u.%u",
        (unsigned)HIWORD(fixed.dwFileVersionMS), (unsigned)LOWORD(fixed.dwFileVersionMS),
        (unsigned)HIWORD(fixed.dwFileVersionLS), (unsigned)LOWORD(fixed.dwFileVersionLS));
}

const char *tb_d3dcompiler_version(const tb_d3dcompiler *compiler)
{
    return compiler->version;
}

/* -------------------------------------------------------------------------
 * Loading.
 * ------------------------------------------------------------------------- */

tb_d3dcompiler_result tb_d3dcompiler_open(tb_d3dcompiler **compiler, tb_d3dcompiler_error *error)
{
    tb_d3dcompiler *const loaded = tb_calloc(1, sizeof(*loaded));
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;
    tb_library_function function = NULL;
    *compiler = NULL;
    if (tb_library_open(LIBRARY_NAME, TB_LIBRARY_SEARCH_SYSTEM, &loaded->library, &library_error) !=
            TB_LIBRARY_OK ||
        tb_library_find_function(loaded->library, "D3DCompile", &function, &library_error) !=
            TB_LIBRARY_OK)
    {
        /* coverage-off: D3DCompiler_47.dll is part of every Windows the engine runs on */
        tb_library_close(loaded->library);
        tb_free(loaded);
        return fail(error, TB_D3DCOMPILER_UNAVAILABLE,
                    "D3DCompiler (" LIBRARY_NAME ") is not in the system directory (%s); it is "
                    "part of Windows 8.1 and later",
                    library_error.message);
        /* coverage-on */
    }
    loaded->compile = (pD3DCompile)function;
    /* The module is found by the function's address, as an address. */
    const void *address = NULL;
    memcpy(&address, &function, sizeof(address));
    describe(loaded, address);
    *compiler = loaded;
    succeed(error);
    return TB_D3DCOMPILER_OK;
}

void tb_d3dcompiler_close(tb_d3dcompiler *compiler)
{
    if (compiler == NULL)
    {
        return;
    }
    tb_library_close(compiler->library);
    tb_free(compiler);
}

/* -------------------------------------------------------------------------
 * Compiling.
 * ------------------------------------------------------------------------- */

static bool is_empty(const char *text)
{
    return text == NULL || text[0] == '\0';
}

static tb_d3dcompiler_result check_request(const tb_d3dcompiler_request *request,
                                           tb_d3dcompiler_error *error)
{
    if (is_empty(request->name) || is_empty(request->profile) || is_empty(request->entry_point))
    {
        return fail(error, TB_D3DCOMPILER_INVALID,
                    "a D3DCompiler compile names its source, its profile and its entry point");
    }
    if (request->source == NULL && request->source_length > 0)
    {
        return fail(error, TB_D3DCOMPILER_INVALID,
                    "a D3DCompiler compile of %zu bytes has no source", request->source_length);
    }
    return TB_D3DCOMPILER_OK;
}

/* Say what failed with the compiler's first line that holds an error; else
 * its first line. */
static void report_first_error(const char *messages, size_t length, tb_d3dcompiler_error *error)
{
    size_t begin = 0;
    size_t end = tb_str_find_char(messages, 0, length, '\n');
    end = end == TB_STR_NOT_FOUND ? length : end;
    for (size_t start = 0; start < length;)
    {
        size_t stop = tb_str_find_char(messages, start, length, '\n');
        stop = stop == TB_STR_NOT_FOUND ? length : stop;
        if (tb_str_find(messages, start, stop, "error") != TB_STR_NOT_FOUND)
        {
            begin = start;
            end = stop;
            break;
        }
        start = stop + 1;
    }
    while (end > begin && messages[end - 1] == '\r')
    {
        end--;
    }
    (void)fail(error, TB_D3DCOMPILER_FAILED, "%.*s", (int)(end - begin), messages + begin);
}

tb_d3dcompiler_result tb_d3dcompiler_compile(tb_d3dcompiler *compiler,
                                             const tb_d3dcompiler_request *request,
                                             tb_d3dcompiler_output *output,
                                             tb_d3dcompiler_error *error)
{
    const tb_d3dcompiler_output empty = {0};
    tb_d3dcompiler_compiled *compiled = NULL;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT status = S_OK;
    tb_d3dcompiler_result result = check_request(request, error);
    *output = empty;
    if (result != TB_D3DCOMPILER_OK)
    {
        return result;
    }
    if ((request->flags & TB_D3DCOMPILER_DEBUG) != 0)
    {
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    }
    else
    {
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    }
    compiled = tb_calloc(1, sizeof(*compiled));
    status = compiler->compile(request->source, request->source_length, request->name, NULL, NULL,
                               request->entry_point, request->profile, flags, 0, &compiled->code,
                               &compiled->messages);
    output->compiled = compiled;
    output->messages = "";
    if (compiled->messages != NULL)
    {
        /* The compiler's messages end in a NUL of their own. */
        output->messages = ID3D10Blob_GetBufferPointer(compiled->messages);
        output->messages_length = tb_str_length(output->messages);
    }
    if (status == E_OUTOFMEMORY)
    {
        /* coverage-off: the compiler runs out of memory only when the system does */
        tb_d3dcompiler_release(output);
        return fail(error, TB_D3DCOMPILER_NO_MEMORY, "D3DCompiler ran out of memory");
        /* coverage-on */
    }
    if (FAILED(status) || compiled->code == NULL)
    {
        report_first_error(output->messages, output->messages_length, error);
        if (error != NULL && error->message[0] == '\0')
        {
            /* coverage-off: a compile that fails says why */
            (void)fail(error, TB_D3DCOMPILER_FAILED, "D3DCompile failed (HRESULT 0x%08lx)",
                       (unsigned long)status);
            /* coverage-on */
        }
        return TB_D3DCOMPILER_FAILED;
    }
    output->code = ID3D10Blob_GetBufferPointer(compiled->code);
    output->code_size = ID3D10Blob_GetBufferSize(compiled->code);
    succeed(error);
    return TB_D3DCOMPILER_OK;
}

void tb_d3dcompiler_release(tb_d3dcompiler_output *output)
{
    tb_d3dcompiler_compiled *const compiled = output->compiled;
    const tb_d3dcompiler_output empty = {0};
    *output = empty;
    if (compiled == NULL)
    {
        return;
    }
    if (compiled->code != NULL)
    {
        (void)ID3D10Blob_Release(compiled->code);
    }
    if (compiled->messages != NULL)
    {
        (void)ID3D10Blob_Release(compiled->messages);
    }
    tb_free(compiled);
}
