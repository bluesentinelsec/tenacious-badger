// A texture and a sampler reached by their bindless indices, as every
// engine shader reaches them, sampled across a quad that covers the target.
struct DrawConstants
{
    uint Texture;
    uint Sampler;
};
ConstantBuffer<DrawConstants> Draw : register(b0);

struct VertexOut
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

VertexOut MainVS(uint id : SV_VertexID)
{
    VertexOut output;
    const float2 corner = float2((id << 1) & 2, id & 2);
    output.Position = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    output.UV = corner;
    return output;
}

float4 MainPS(VertexOut input) : SV_Target
{
    Texture2D<float4> texture = ResourceDescriptorHeap[Draw.Texture];
    SamplerState pointSampler = SamplerDescriptorHeap[Draw.Sampler];
    return texture.Sample(pointSampler, input.UV);
}
