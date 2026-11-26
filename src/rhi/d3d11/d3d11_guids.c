/* The interface identifiers of D3D11, defined once here instead of linking
 * dxguid.lib: initguid.h makes each DEFINE_GUID a definition. DXGI's are
 * defined with D3D12's, in d3d12_guids.c. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <initguid.h>

#include <d3d11_1.h>
#include <d3d11sdklayers.h>
