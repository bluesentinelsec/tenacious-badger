// dxc: -T hs_6_6 -E main
struct CP { float3 p : POSITION; };
struct PC { float2 a : A; float e[2] : SV_TessFactor;  float3 b : B; nointerpolation uint c : C; float d : D; };
PC patch(InputPatch<CP, 3> ip) { PC o = (PC)0; o.e[0] = ip[0].p.x; return o; }
[domain("isoline")] [partitioning("integer")] [outputtopology("line")] [outputcontrolpoints(3)] [patchconstantfunc("patch")]
CP main(InputPatch<CP, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }
