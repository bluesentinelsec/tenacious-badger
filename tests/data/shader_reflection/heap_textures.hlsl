// dxc: -T ps_6_6 -E MainPS
// Textures and a sampler taken from the descriptor heaps by the indices a
// constant buffer holds: the heaps bind nothing, so only the buffer is bound.
cbuffer Draw : register(b0)
{
    uint Texture;
    uint Sampler;
    float Angle;
    float Aspect;
};

struct VertexOut
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
    nointerpolation uint Layer : LAYER;
};

float4 MainPS(VertexOut input) : SV_Target
{
    Texture2D<float4> image = ResourceDescriptorHeap[Texture];
    SamplerState sampler = SamplerDescriptorHeap[Sampler];
    float4 color = image.Sample(sampler, input.UV);

    Texture2DArray<float4> layers;
    layers = ResourceDescriptorHeap[Texture + 1];
    color += layers.SampleLevel(sampler, float3(input.UV, input.Layer), 0);

    TextureCube<float4> sky = ResourceDescriptorHeap[NonUniformResourceIndex(input.Layer + 2)];
    color += sky.Sample(sampler, float3(input.UV, Aspect));

    Texture3D<float> volume = ResourceDescriptorHeap[(int)Angle];
    color.x += volume.Load(int4(int2(input.Position.xy), 0, 0));
    color.y += image.mips[1][uint2(input.Position.xy)].y;
    return color;
}
