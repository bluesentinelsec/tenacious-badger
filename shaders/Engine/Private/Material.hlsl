// The tangent frame of each pixel comes from the screen-space derivatives
// of its position and texture coordinates (Schueler, "Normal Mapping Without
// Precomputed Tangents", 2013), so a mesh needs no tangents: any caller's
// vertices work, mirrored coordinates included, for a few instructions a
// pixel. Its sign follows the screen's handedness, since D3D's y runs down
// where the paper's runs up. A normal map's +Y is up in the image, toward
// smaller v, as glTF's is. The surface is written to the G-buffer, which
// the lighting pass lights: its base color; its normal, octahedrally
// encoded (Cigolle et al., "A Survey of Efficient Representations for
// Independent Unit Vectors", 2014); and its metallic, roughness, ambient
// occlusion, and whether it is lit.
struct MaterialConstants
{
    float4x4 Clip; float4x4 Model; float4 Color; float4 Surface; float4 UV; uint4 Textures;
};
ConstantBuffer<MaterialConstants> Draw : register(b1);
struct VertexIn { float3 Position : POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD0; };
struct VertexOut
{
    float4 Position : SV_Position;
    float3 World : TEXCOORD1;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};
struct GBuffer
{
    float4 BaseColor : SV_Target0;
    float2 Normal : SV_Target1;
    float4 Surface : SV_Target2;
};
VertexOut MainVS(VertexIn input)
{
    VertexOut output;
    output.Position = mul(Draw.Clip, float4(input.Position, 1));
    output.World = mul(Draw.Model, float4(input.Position, 1)).xyz;
    output.Normal = mul(Draw.Model, float4(input.Normal, 0)).xyz;
    output.UV = input.UV * Draw.UV.xy;
    return output;
}
float3 BendNormal(float3 normal, float3 position, float2 uv, float3 bent)
{
    const float3 dp1 = ddx(position);
    const float3 dp2 = ddy(position);
    const float2 duv1 = ddx(uv);
    const float2 duv2 = ddy(uv);
    const float3 dp2perp = cross(dp2, normal);
    const float3 dp1perp = cross(normal, dp1);
    const float3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
    const float3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
    const float largest = max(dot(tangent, tangent), dot(bitangent, bitangent));
    const float handed = dot(cross(dp1, dp2), normal) < 0.0 ? -1.0 : 1.0;
    const float scale = rsqrt(max(largest, 1e-20)) * handed;
    const float3 bent_normal =
        tangent * (bent.x * scale) - bitangent * (bent.y * scale) + normal * bent.z;
    return normalize(bent_normal);
}
float2 EncodeNormal(float3 normal)
{
    const float2 folded = normal.xy / (abs(normal.x) + abs(normal.y) + abs(normal.z));
    if (normal.z >= 0.0)
    {
        return folded;
    }
    const float2 sign_of = float2(folded.x >= 0.0 ? 1.0 : -1.0, folded.y >= 0.0 ? 1.0 : -1.0);
    return (1.0 - abs(folded.yx)) * sign_of;
}
GBuffer MainPS(VertexOut input)
{
    Texture2D<float4> base_map = ResourceDescriptorHeap[Draw.Textures.x];
    Texture2D<float4> normal_map = ResourceDescriptorHeap[Draw.Textures.y];
    Texture2D<float4> surface_map = ResourceDescriptorHeap[Draw.Textures.z];
    SamplerState filter = SamplerDescriptorHeap[Draw.Textures.w];
    const float4 base = base_map.Sample(filter, input.UV) * Draw.Color;
    float3 bent = normal_map.Sample(filter, input.UV).xyz * 2.0 - 1.0;
    bent.xy *= Draw.Surface.z;
    const float3 normal =
        BendNormal(normalize(input.Normal), input.World, input.UV, bent);
    const float4 surface = surface_map.Sample(filter, input.UV);
    GBuffer output;
    output.BaseColor = float4(saturate(base.rgb), 1);
    output.Normal = EncodeNormal(normal);
    output.Surface = float4(saturate(surface.b * Draw.Surface.y),
                            saturate(surface.g * Draw.Surface.x), 1, Draw.Surface.w);
    return output;
}
