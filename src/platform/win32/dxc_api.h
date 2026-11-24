/* Private to the Win32 adapter: the part of DXC's COM interface the engine
 * calls, declared for C.
 *
 * The Windows SDK's dxcapi.h declares these interfaces only for C++ (as
 * classes that derive from IUnknown), so they are declared here as C COM
 * declares any interface: a struct whose first member points to a table of
 * functions, IUnknown's three first, then the base interfaces', then the
 * interface's own, in the order of dxcapi.h. Only the functions the engine
 * calls are named; a table stops after the last of them, which is safe
 * because nothing past it is reached. The GUIDs and values are dxcapi.h's,
 * from the Windows SDK 10.0.26100. */
#ifndef TB_WIN32_DXC_API_H
#define TB_WIN32_DXC_API_H

#include <windows.h>

/* dxcapi.h's DXC_CP_UTF8: text in UTF-8. */
#define TB_DXC_CP_UTF8 65001u

/* dxcapi.h's DXC_OUT_KIND values this adapter asks for. */
#define TB_DXC_OUT_OBJECT 1u
#define TB_DXC_OUT_ERRORS 2u

/* {73e22d93-e6ce-47f3-b5bf-f0664f39c1b0} */
static const GUID tb_dxc_clsid_compiler = {
    0x73e22d93, 0xe6ce, 0x47f3, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
/* {228B4687-5A6A-4730-900C-9702B2203F54} */
static const GUID tb_dxc_iid_compiler3 = {
    0x228b4687, 0x5a6a, 0x4730, {0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54}};
/* {58346CDA-DDE7-4497-9461-6F87AF5E0659} */
static const GUID tb_dxc_iid_result = {
    0x58346cda, 0xdde7, 0x4497, {0x94, 0x61, 0x6f, 0x87, 0xaf, 0x5e, 0x06, 0x59}};
/* {8BA5FB08-5195-40e2-AC58-0D989C3A0102} */
static const GUID tb_dxc_iid_blob = {
    0x8ba5fb08, 0x5195, 0x40e2, {0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02}};
/* {3DA636C9-BA71-4024-A301-30CBF125305B} */
static const GUID tb_dxc_iid_blob_utf8 = {
    0x3da636c9, 0xba71, 0x4024, {0xa3, 0x01, 0x30, 0xcb, 0xf1, 0x25, 0x30, 0x5b}};

/* {8CA3E215-F728-4CF3-8CDD-88AF917587A1}: the validator, in dxil.dll. */
static const GUID tb_dxc_clsid_validator = {
    0x8ca3e215, 0xf728, 0x4cf3, {0x8c, 0xdd, 0x88, 0xaf, 0x91, 0x75, 0x87, 0xa1}};
/* {b04f5b50-2059-4f12-a8ff-a1e0cde1cc7e} */
static const GUID tb_dxc_iid_version_info = {
    0xb04f5b50, 0x2059, 0x4f12, {0xa8, 0xff, 0xa1, 0xe0, 0xcd, 0xe1, 0xcc, 0x7e}};
/* {fb6904c4-42f0-4b62-9c46-983af7da7c83} */
static const GUID tb_dxc_iid_version_info2 = {
    0xfb6904c4, 0x42f0, 0x4b62, {0x9c, 0x46, 0x98, 0x3a, 0xf7, 0xda, 0x7c, 0x83}};

/* IDxcVersionInfo, and IDxcVersionInfo2, which derives from it: the version
 * of the compiler or the validator, and the commit it was built from. */
typedef struct tb_dxc_version_info tb_dxc_version_info;
typedef struct tb_dxc_version_info_functions
{
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(tb_dxc_version_info *self, REFIID iid,
                                               void **object);
    ULONG(STDMETHODCALLTYPE *AddRef)(tb_dxc_version_info *self);
    ULONG(STDMETHODCALLTYPE *Release)(tb_dxc_version_info *self);
    HRESULT(STDMETHODCALLTYPE *GetVersion)(tb_dxc_version_info *self, UINT32 *major, UINT32 *minor);
    HRESULT(STDMETHODCALLTYPE *GetFlags)(tb_dxc_version_info *self, UINT32 *flags);
    /* IDxcVersionInfo2; the hash is the caller's, to free with CoTaskMemFree(). */
    HRESULT(STDMETHODCALLTYPE *GetCommitInfo)(tb_dxc_version_info *self, UINT32 *count,
                                              char **hash);
} tb_dxc_version_info_functions;
struct tb_dxc_version_info
{
    const tb_dxc_version_info_functions *lpVtbl;
};

/* dxcapi.h's DxcBuffer: bytes or text given to DXC. */
typedef struct tb_dxc_buffer
{
    LPCVOID pointer;
    SIZE_T size;
    UINT encoding;
} tb_dxc_buffer;

/* IDxcBlob: bytes DXC gives. IDxcBlobUtf8 derives from it, through
 * IDxcBlobEncoding, and adds a NUL-terminated UTF-8 string. */
typedef struct tb_dxc_blob tb_dxc_blob;
typedef struct tb_dxc_blob_functions
{
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(tb_dxc_blob *self, REFIID iid, void **object);
    ULONG(STDMETHODCALLTYPE *AddRef)(tb_dxc_blob *self);
    ULONG(STDMETHODCALLTYPE *Release)(tb_dxc_blob *self);
    LPVOID(STDMETHODCALLTYPE *GetBufferPointer)(tb_dxc_blob *self);
    SIZE_T(STDMETHODCALLTYPE *GetBufferSize)(tb_dxc_blob *self);
    /* IDxcBlobEncoding */
    HRESULT(STDMETHODCALLTYPE *GetEncoding)(tb_dxc_blob *self, BOOL *known, UINT32 *code_page);
    /* IDxcBlobUtf8 */
    LPCSTR(STDMETHODCALLTYPE *GetStringPointer)(tb_dxc_blob *self);
    SIZE_T(STDMETHODCALLTYPE *GetStringLength)(tb_dxc_blob *self);
} tb_dxc_blob_functions;
struct tb_dxc_blob
{
    const tb_dxc_blob_functions *lpVtbl;
};

/* IDxcResult, which derives from IDxcOperationResult. */
typedef struct tb_dxc_result_object tb_dxc_result_object;
typedef struct tb_dxc_result_functions
{
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(tb_dxc_result_object *self, REFIID iid,
                                               void **object);
    ULONG(STDMETHODCALLTYPE *AddRef)(tb_dxc_result_object *self);
    ULONG(STDMETHODCALLTYPE *Release)(tb_dxc_result_object *self);
    /* IDxcOperationResult */
    HRESULT(STDMETHODCALLTYPE *GetStatus)(tb_dxc_result_object *self, HRESULT *status);
    HRESULT(STDMETHODCALLTYPE *GetResult)(tb_dxc_result_object *self, tb_dxc_blob **result);
    HRESULT(STDMETHODCALLTYPE *GetErrorBuffer)(tb_dxc_result_object *self, tb_dxc_blob **errors);
    /* IDxcResult */
    BOOL(STDMETHODCALLTYPE *HasOutput)(tb_dxc_result_object *self, UINT kind);
    HRESULT(STDMETHODCALLTYPE *GetOutput)(tb_dxc_result_object *self, UINT kind, REFIID iid,
                                          void **object, void **name);
} tb_dxc_result_functions;
struct tb_dxc_result_object
{
    const tb_dxc_result_functions *lpVtbl;
};

/* IDxcCompiler3. */
typedef struct tb_dxc_compiler tb_dxc_compiler;
typedef struct tb_dxc_compiler_functions
{
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(tb_dxc_compiler *self, REFIID iid, void **object);
    ULONG(STDMETHODCALLTYPE *AddRef)(tb_dxc_compiler *self);
    ULONG(STDMETHODCALLTYPE *Release)(tb_dxc_compiler *self);
    HRESULT(STDMETHODCALLTYPE *Compile)(tb_dxc_compiler *self, const tb_dxc_buffer *source,
                                        LPCWSTR *arguments, UINT32 argument_count,
                                        void *include_handler, REFIID iid, LPVOID *result);
} tb_dxc_compiler_functions;
struct tb_dxc_compiler
{
    const tb_dxc_compiler_functions *lpVtbl;
};

/* DxcCreateInstance, which dxcompiler.dll and dxil.dll export. */
typedef HRESULT(__stdcall *tb_dxc_create_instance_function)(REFCLSID clsid, REFIID iid,
                                                            LPVOID *object);

#endif
