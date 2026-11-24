// dxc: -T vs_6_6 -E main
struct O { nointerpolation uint x : X; uint rt : SV_RenderTargetArrayIndex; nointerpolation uint y : Y; float4 p : SV_Position; float cd : SV_ClipDistance0; float cd1 : SV_ClipDistance1; float cl : SV_CullDistance; float a : A; uint vp : SV_ViewportArrayIndex; nointerpolation uint z : Z; };
O main(float4 p : POSITION) { O o = (O)0; o.p = p; return o; }
