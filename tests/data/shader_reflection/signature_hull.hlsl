// dxc: -T hs_6_6 -E main
struct CP { float3 p : POSITION; float2 uv : TEXCOORD0; };
struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };
PC patch(InputPatch<CP, 3> ip, uint pid : SV_PrimitiveID) { PC o = (PC)0; o.e[0] = ip[0].p.x + pid; return o; }
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)] [patchconstantfunc("patch")]
CP main(InputPatch<CP, 3> ip, uint i : SV_OutputControlPointID, uint pid : SV_PrimitiveID) { CP o = ip[i]; o.uv += pid; return o; }
