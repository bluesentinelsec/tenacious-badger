// The viewport render mode's upscale: the viewport's target, a pixel a unit,
// drawn into the viewport's rectangle of the swap chain, whose top left is
// Origin. The rectangle shows the target's units from 0 to Extent of its
// width and height; each pixel samples the target there, kept half a texel
// inside what is shown, so that a linear filter never reads past its edge.
// The sampler is nearest at whole pixels a unit, linear otherwise. The
// target holds what the swap chain would, so it is copied as it is.
struct UpscaleConstants
{
    uint Source; uint Sampler; float2 Origin; float2 Inverse; float2 Extent; float2 Low;
    float2 High;
};
ConstantBuffer<UpscaleConstants> Upscale : register(b0);
struct VertexOut { float4 Position : SV_Position; };
VertexOut MainVS(uint vertex : SV_VertexID)
{
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    VertexOut output;
    output.Position = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}
float4 MainPS(VertexOut input) : SV_Target
{
    Texture2D<float4> source = ResourceDescriptorHeap[Upscale.Source];
    SamplerState filter = SamplerDescriptorHeap[Upscale.Sampler];
    const float2 shown = (input.Position.xy - Upscale.Origin) * Upscale.Inverse;
    const float2 uv = clamp(shown * Upscale.Extent, Upscale.Low, Upscale.High);
    return float4(source.SampleLevel(filter, uv, 0).rgb, 1);
}
