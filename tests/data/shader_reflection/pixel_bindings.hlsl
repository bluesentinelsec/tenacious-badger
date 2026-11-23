// dxc: -T ps_6_6 -E main
Texture2D unused_tex;
Texture2D albedo;
Texture2D normals : register(t0);
Texture2D extra;
SamplerState s;
SamplerComparisonState shadow : register(s1); Texture2D<float> depth;
RWTexture2D<float4> output;
StructuredBuffer<float4> data : register(t2, space1);
Texture2D<float> arr[4];
cbuffer Frame { float4 tint; float time; };
float4 loose;

struct PSIn
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint id : ID;
    float3 normal[2] : NORMAL;
};

struct PSOut
{
    float4 color : SV_Target0;
    float4 other : SV_Target2;
};

PSOut main(PSIn i, bool front : SV_IsFrontFace)
{
    PSOut o;
    o.color = albedo.Sample(s, i.uv) * tint + normals.Sample(s, i.uv) + extra.Sample(s, i.uv) * time + loose;
    o.color += data[i.id] + arr[i.id % 4].Sample(s, i.uv) + depth.SampleCmp(shadow, i.uv, 0.5);
    o.other = float4(i.normal[1], front ? 1 : 0);
    output[uint2(0, 0)] = o.color;
    return o;
}
