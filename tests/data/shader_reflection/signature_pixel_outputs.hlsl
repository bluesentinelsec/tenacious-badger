// dxc: -T ps_6_6 -E main
struct O { float2 t2 : SV_Target2; float4 t0 : SV_Target0; float d : SV_Depth; uint c : SV_Coverage; uint s : SV_StencilRef; };
O main(float4 p : SV_Position) { O o = (O)0; o.t2 = p.xy; o.t0 = p; o.d = p.z; o.c = 1; o.s = 2; return o; }
