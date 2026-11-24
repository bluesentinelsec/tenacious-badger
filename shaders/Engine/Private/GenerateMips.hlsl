// Each thread makes a texel of a level from the 2x2 texels of the level
// before, clamped at its edges: colors averaged in linear light and written
// back in sRGB, normals averaged and renormalized. Both levels are written
// through their storage views, so the texture stays in one state.
struct MipConstants
{
    uint Source; uint Dest; uint Width; uint Height;
    uint SourceWidth; uint SourceHeight; uint Content; uint Array;
};
ConstantBuffer<MipConstants> Mip : register(b0);
float3 ToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}
float3 ToSRGB(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}
float4 Decode(float4 texel)
{
    if (Mip.Content == 0)
    {
        return float4(ToLinear(texel.rgb), texel.a);
    }
    if (Mip.Content == 2)
    {
        return float4(texel.rgb * 2.0 - 1.0, texel.a);
    }
    return texel;
}
float4 Encode(float4 value)
{
    if (Mip.Content == 0)
    {
        return float4(ToSRGB(saturate(value.rgb)), value.a);
    }
    if (Mip.Content == 2)
    {
        const float size = length(value.rgb);
        const float3 normal = size > 0.0 ? value.rgb / size : float3(0, 0, 1);
        return float4(normal * 0.5 + 0.5, value.a);
    }
    return value;
}
float4 Load(uint2 at, uint layer)
{
    if (Mip.Array != 0)
    {
        RWTexture2DArray<float4> layers = ResourceDescriptorHeap[Mip.Source];
        return layers[uint3(at, layer)];
    }
    RWTexture2D<float4> level = ResourceDescriptorHeap[Mip.Source];
    return level[at];
}
void Store(uint2 at, uint layer, float4 value)
{
    if (Mip.Array != 0)
    {
        RWTexture2DArray<float4> layers = ResourceDescriptorHeap[Mip.Dest];
        layers[uint3(at, layer)] = value;
        return;
    }
    RWTexture2D<float4> level = ResourceDescriptorHeap[Mip.Dest];
    level[at] = value;
}
[numthreads(8, 8, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Mip.Width || id.y >= Mip.Height)
    {
        return;
    }
    const uint2 last = uint2(Mip.SourceWidth - 1, Mip.SourceHeight - 1);
    const uint2 at = id.xy * 2;
    float4 sum = Decode(Load(min(at, last), id.z));
    sum += Decode(Load(min(at + uint2(1, 0), last), id.z));
    sum += Decode(Load(min(at + uint2(0, 1), last), id.z));
    sum += Decode(Load(min(at + uint2(1, 1), last), id.z));
    Store(id.xy, id.z, Encode(sum * 0.25));
}
