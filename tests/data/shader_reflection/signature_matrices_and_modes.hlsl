// dxc: -T ps_6_6 -E main
struct I { float3x2 m : M; row_major float3x2 rm : RM; float2x3 m23 : MB; float2x2 ma[2] : MA; float2 c[2] : COLOR; float d : D; float2 e : E; centroid float f : F; noperspective float g : G; float h : H; sample float k : K; };
float4 main(I i) : SV_Target { return i.m[0][0] + i.rm[0][0] + i.m23[0][0] + i.ma[1][0][0] + i.c[1].x + i.d + i.e.x + i.f + i.g + i.h + i.k; }
