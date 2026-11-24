// dxc: -T ps_6_6 -E main
struct I { float2 a : A; uint prim : SV_PrimitiveID; nointerpolation uint x : X; float4 p : SV_Position; uint rt : SV_RenderTargetArrayIndex; nointerpolation uint y : Y; float b : B; uint vp : SV_ViewportArrayIndex; };
float4 main(I i, uint cov : SV_Coverage) : SV_Target { return i.a.x + i.prim + i.x + i.p.x + i.rt + i.y + i.b + i.vp + cov; }
