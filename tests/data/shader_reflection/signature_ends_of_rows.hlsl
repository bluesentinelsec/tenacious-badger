// dxc: -T ps_6_6 -E main
struct I { nointerpolation uint x : X; uint prim : SV_PrimitiveID; nointerpolation uint y : Y; nointerpolation uint2 z : Z; float a : A; float4 p : SV_Position; };
float4 main(I i) : SV_Target { return i.x + i.prim + i.y + i.z.x + i.a + i.p.x; }
