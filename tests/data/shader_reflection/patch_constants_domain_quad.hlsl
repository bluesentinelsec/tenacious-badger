// dxc: -T ds_6_6 -E main
struct CP { float3 p : POSITION; };
struct PC { float2 a : A; float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; float3 b : B; };
[domain("quad")] float4 main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<CP, 4> cp) : SV_Position { return float4(cp[0].p * uv.x, pc.a.x + pc.b.x + pc.e[1]); }
