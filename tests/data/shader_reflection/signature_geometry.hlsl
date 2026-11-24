// dxc: -T gs_6_6 -E main
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
struct G { float4 p : SV_Position; float2 uv : TEXCOORD0; uint rt : SV_RenderTargetArrayIndex; uint prim : SV_PrimitiveID; };
[maxvertexcount(3)] void main(triangle V v[3], uint pid : SV_PrimitiveID, uint gi : SV_GSInstanceID, inout TriangleStream<G> s) { G g = (G)0; g.p = v[0].p; g.uv = v[0].uv; g.rt = pid + gi; g.prim = pid; s.Append(g); }
