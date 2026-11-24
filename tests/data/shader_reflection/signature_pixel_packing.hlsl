// dxc: -T ps_6_6 -E main
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float  a : TEXCOORD1;
  nointerpolation uint id : ID;
  float3 n : NORMAL;
  float  b : TEXCOORD2;
  float2 c[2] : COLOR;
  float  d : TEXCOORD3;
  float3x2 m : MAT;
  bool f : FACE_FLAG;
  uint prim : SV_PrimitiveID;
  float cd : SV_ClipDistance0;
  float cl : SV_CullDistance0;
};
float4 main(VSOut i, bool ff : SV_IsFrontFace, uint s : SV_SampleIndex) : SV_Target { return i.pos + i.uv.x + i.a + i.id + i.n.x + i.b + i.c[1].x + i.d + i.m[0][0] + i.f + i.prim + i.cd + i.cl + ff + s; }
