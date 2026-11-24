// dxc: -T ps_6_6 -E main
struct I { noperspective centroid float a : A; noperspective sample float b : B; float2 c : C; uint sr : SV_ShadingRate; nointerpolation uint d : D; uint prim : SV_PrimitiveID; };
float4 main(I i) : SV_Target { return i.a + i.b + i.c.x + i.sr + i.d + i.prim; }
