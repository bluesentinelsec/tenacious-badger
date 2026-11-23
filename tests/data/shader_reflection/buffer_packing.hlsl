// dxc: -T ps_6_6 -E main
struct S3 { float3 v; };
cbuffer M { float a; min16float b; float c; min16float2 d; float e; min16int f; float g; S3 arr[2]; float h; float2 i[2]; float j; };
float4 main() : SV_Target { return a + b + c + d.x + e + f + g + arr[1].v.x + h + i[1].x + j; }
