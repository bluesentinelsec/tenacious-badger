// The tangent frame of each pixel comes from the screen-space derivatives
// of its position and texture coordinates (Schueler, "Normal Mapping Without
// Precomputed Tangents", 2013), so a mesh needs no tangents: any caller's
// vertices work, mirrored coordinates included, for a few instructions a
// pixel. Its sign follows the screen's handedness, since D3D's y runs down
// where the paper's runs up. A normal map's +Y is up in the image, toward smaller v, as glTF's
// is. Shade() is the lighting of a surface: the renderer's one light with
// ambient, and a highlight that roughness and metallic shape, until the
// raster lighting mode replaces it.
struct MaterialConstants
{
    float4x4 Clip; float4x4 Model; float4 Color; float4 Light; float4 Eye;
    float4 Surface; float4 UV; uint4 Textures;
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
float3 Shade(float3 base, float3 normal, float3 view, float roughness, float metallic)
{
    const float3 to_light = -Draw.Light.xyz;
    const float facing = saturate(dot(normal, to_light));
    const float3 diffuse = base * (1.0 - metallic) * (0.25 + 0.75 * facing);
    const float3 specular = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    const float3 halfway = normalize(to_light + view);
    const float shininess = exp2(10.0 * (1.0 - roughness) + 1.0);
    const float highlight =
        pow(saturate(dot(normal, halfway)), shininess) * (shininess + 2.0) / 8.0 * facing;
    return diffuse + specular * (0.25 * metallic + highlight);
}
float4 MainPS(VertexOut input) : SV_Target
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
    const float roughness = saturate(surface.g * Draw.Surface.x);
    const float metallic = saturate(surface.b * Draw.Surface.y);
    const float3 view = normalize(Draw.Eye.xyz - input.World);
    const float3 lit = Shade(base.rgb, normal, view, roughness, metallic);
    return float4(lerp(base.rgb, lit, Draw.Light.w), base.a);
}
