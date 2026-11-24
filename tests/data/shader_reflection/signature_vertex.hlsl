// dxc: -T vs_6_6 -E main
struct O { float2 a : A; float4 p : SV_Position; float b : B; float2 cd : SV_ClipDistance; float3 cl : SV_CullDistance; uint vi : SV_ViewportArrayIndex; float c : C; };
O main(float3 pos : POSITION, float2 uv : TEXCOORD0, uint id : SV_VertexID, float w : W, uint inst : SV_InstanceID) { O o = (O)0; o.p = float4(pos, w + uv.x + id + inst); return o; }
