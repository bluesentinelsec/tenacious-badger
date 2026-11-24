// dxc: -T ms_6_6 -E main
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; nointerpolation uint id : ID; float c : C; };
struct P { uint prim : SV_PrimitiveID; bool cull : SV_CullPrimitive; uint rt : SV_RenderTargetArrayIndex; float3 n : NORMAL; uint m : MAT; };
struct Payload { uint x; };
[outputtopology("triangle")] [numthreads(32, 1, 1)]
void main(uint t : SV_GroupThreadID, in payload Payload pl, out vertices V verts[3], out indices uint3 tris[1], out primitives P prims[1]) {
  SetMeshOutputCounts(3, 1);
  if (t < 3) { V v = (V)0; v.uv = pl.x; verts[t] = v; }
  if (t == 0) { tris[0] = uint3(0, 1, 2); P p = (P)0; p.n = 1; prims[0] = p; }
}
