// dxc: -T ps_6_6 -E main
float2x4 before;
#pragma pack_matrix(row_major)
struct S { float2x4 m; float x; };
cbuffer P { float2x4 a; float b; column_major float2x4 c; float d; S s; float3x2 g[2]; float h; };
#pragma pack_matrix( column_major )
cbuffer Q { float2x4 e; float f; row_major float2x4 k; float l; };
float4 main() : SV_Target { return before[0] + a[0] + b + c[0] + d + s.m[0] + s.x + g[1][0].x + h + e[0] + f + k[0] + l; }
