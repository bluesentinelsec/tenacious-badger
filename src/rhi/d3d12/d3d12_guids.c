/* The interface identifiers of D3D12 and DXGI, defined once here instead of
 * linking dxguid.lib: initguid.h makes each DEFINE_GUID a definition. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <initguid.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
