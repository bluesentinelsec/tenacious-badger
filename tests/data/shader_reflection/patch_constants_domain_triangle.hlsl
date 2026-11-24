// dxc: -T ds_6_6 -E main
struct CP { float3 p : POSITION; float2 uv : TEXCOORD0; };
struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };
[domain("tri")] float4 main(PC pc, float3 bary : SV_DomainLocation, const OutputPatch<CP, 3> cp, uint pid : SV_PrimitiveID) : SV_Position { return float4(cp[0].p * bary.x, pc.i + pid); }
