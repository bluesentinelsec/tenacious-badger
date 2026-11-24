// dxc: -T ps_6_6 -E main
struct I { float4 p : SV_Position; float b[3] : B; float2 c : C; float d : D; float3 e : E; };
float4 main(I i, float3 bary : SV_Barycentrics, uint sr : SV_ShadingRate) : SV_Target { return i.p + i.b[2] + i.c.x + i.d + i.e.x + bary.x + sr; }
