// The output transform: exposure, then Narkowicz's fit of the ACES filmic
// curve to each channel, with the 0.6 he scales its input by to match
// ACES's reference (a middle gray of 0.18 shows as about 0.41 in sRGB),
// then sRGB's encoding, since the swap chain is UNORM and the display reads
// it as sRGB.
struct ToneConstants { uint Scene; float Exposure; uint2 Unused; };
ConstantBuffer<ToneConstants> Tone : register(b0);
struct VertexOut { float4 Position : SV_Position; };
VertexOut MainVS(uint vertex : SV_VertexID)
{
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    VertexOut output;
    output.Position = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}
float3 FilmicACES(float3 color)
{
    const float3 x = color * 0.6;
    return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));
}
float3 ToSRGB(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}
float4 MainPS(VertexOut input) : SV_Target
{
    Texture2D<float4> scene = ResourceDescriptorHeap[Tone.Scene];
    const float3 color = max(scene.Load(int3(input.Position.xy, 0)).rgb, 0.0);
    return float4(ToSRGB(FilmicACES(color * Tone.Exposure)), 1);
}
