// Unreal's default lit shading model: a Lambert diffuse, and a specular of
// GGX's distribution, Smith's joint visibility (approximated), and
// Schlick's Fresnel, with Unreal's remapping of roughness (a = roughness
// squared). Each pixel's place in the world comes back from its depth
// through the inverse of the frame's clip transform. Ambient light comes
// from the sky: a cube map's lower levels, or the colors of a procedural
// sky (or of the ambient light without one), blended toward the average of
// the half they face as a surface roughens; reflections are scaled by
// Karis's analytical fit of the environment BRDF; without reflections, the
// specular takes the ambient light the surface faces, so that metals do not
// go black. Point and spot lights
// fall off by the inverse square of their distance, windowed to nothing at
// their range; a spot's cone fades between its angles, squared, as
// Unreal's do.
#define PI 3.14159265
struct GBufferIndices { uint BaseColor; uint Normal; uint Surface; uint Depth; };
ConstantBuffer<GBufferIndices> GBuffer : register(b0);
struct LightingConstants
{
    float4x4 ClipToWorld; float4 Eye; float4 Forward; float4 Clear;
    float4 Zenith; float4 Horizon; float4 Ground; float4 Size; uint4 Lights; uint4 Sky;
};
ConstantBuffer<LightingConstants> Lighting : register(b1);
struct VertexOut { float4 Position : SV_Position; };
VertexOut MainVS(uint vertex : SV_VertexID)
{
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    VertexOut output;
    output.Position = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}
float3 DecodeNormal(float2 encoded)
{
    float3 normal = float3(encoded.x, encoded.y, 1.0 - abs(encoded.x) - abs(encoded.y));
    const float fold = saturate(-normal.z);
    normal.x += normal.x >= 0.0 ? -fold : fold;
    normal.y += normal.y >= 0.0 ? -fold : fold;
    return normalize(normal);
}
float Pow5(float x)
{
    const float x2 = x * x;
    return x2 * x2 * x;
}
float DistributionGGX(float a2, float n_dot_h)
{
    const float d = (n_dot_h * a2 - n_dot_h) * n_dot_h + 1.0;
    return a2 / (PI * d * d);
}
float VisibilitySmithJoint(float a2, float n_dot_v, float n_dot_l)
{
    const float a = sqrt(a2);
    const float smith_v = n_dot_l * (n_dot_v * (1.0 - a) + a);
    const float smith_l = n_dot_v * (n_dot_l * (1.0 - a) + a);
    return 0.5 / (smith_v + smith_l);
}
float3 FresnelSchlick(float3 specular, float v_dot_h)
{
    const float fc = Pow5(1.0 - v_dot_h);
    return saturate(50.0 * specular.g) * fc + (1.0 - fc) * specular;
}
float3 EnvironmentBRDF(float3 specular, float roughness, float n_dot_v)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return specular * ab.x + saturate(50.0 * specular.g) * ab.y;
}
float3 SkyLight(float3 direction, float blur)
{
    if (Lighting.Sky.x != 0)
    {
        TextureCube<float4> cube = ResourceDescriptorHeap[Lighting.Sky.y];
        SamplerState filter = SamplerDescriptorHeap[Lighting.Sky.z];
        const float level = blur * (float)(Lighting.Sky.w - 1);
        return cube.SampleLevel(filter, direction, level).rgb;
    }
    const float up = direction.y;
    float3 sharp = lerp(Lighting.Horizon.rgb, Lighting.Ground.rgb, saturate(-up * 12.0));
    if (up >= 0.0)
    {
        sharp = lerp(Lighting.Horizon.rgb, Lighting.Zenith.rgb, sqrt(up));
    }
    const float3 above = 0.5 * (Lighting.Horizon.rgb + Lighting.Zenith.rgb);
    const float3 soft = lerp(Lighting.Ground.rgb, above, saturate(0.5 + 0.5 * up));
    return lerp(sharp, soft, blur);
}
float3 Shade(float3 diffuse, float3 specular, float a2, float3 normal, float3 view,
             float3 to_light, float n_dot_v)
{
    const float n_dot_l = saturate(dot(normal, to_light));
    if (n_dot_l <= 0.0)
    {
        return float3(0, 0, 0);
    }
    const float3 halfway = normalize(view + to_light);
    const float n_dot_h = saturate(dot(normal, halfway));
    const float v_dot_h = saturate(dot(view, halfway));
    const float3 lobe = DistributionGGX(a2, n_dot_h) *
                        VisibilitySmithJoint(a2, n_dot_v, n_dot_l) *
                        FresnelSchlick(specular, v_dot_h);
    return (diffuse / PI + lobe) * n_dot_l;
}
float4 MainPS(VertexOut input) : SV_Target
{
    const int3 texel = int3(input.Position.xy, 0);
    Texture2D<float> depth_map = ResourceDescriptorHeap[GBuffer.Depth];
    const float depth = depth_map.Load(texel);
    if (depth <= 0.0)
    {
        return float4(Lighting.Clear.rgb, 1);
    }
    Texture2D<float4> base_map = ResourceDescriptorHeap[GBuffer.BaseColor];
    Texture2D<float2> normal_map = ResourceDescriptorHeap[GBuffer.Normal];
    Texture2D<float4> surface_map = ResourceDescriptorHeap[GBuffer.Surface];
    const float3 base = base_map.Load(texel).rgb;
    const float4 surface = surface_map.Load(texel);
    if (surface.a < 0.5)
    {
        return float4(base, 1);
    }
    const float3 normal = DecodeNormal(normal_map.Load(texel));
    const float2 ndc = float2(input.Position.x * Lighting.Size.z * 2.0 - 1.0,
                              1.0 - input.Position.y * Lighting.Size.w * 2.0);
    const float4 place = mul(Lighting.ClipToWorld, float4(ndc, depth, 1));
    const float3 world = place.xyz / place.w;
    float3 view = -Lighting.Forward.xyz;
    if (Lighting.Eye.w != 0.0)
    {
        view = normalize(Lighting.Eye.xyz - world);
    }
    const float n_dot_v = saturate(abs(dot(normal, view)) + 1e-5);
    const float metallic = surface.r;
    const float roughness = max(surface.g, 0.02);
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float3 diffuse = base * (1.0 - metallic);
    const float3 specular = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    float3 color = float3(0, 0, 0);
    ByteAddressBuffer lights = ResourceDescriptorHeap[Lighting.Lights.x];
    for (uint index = 0; index < Lighting.Lights.z; index++)
    {
        const uint at = Lighting.Lights.y + index * 64;
        const float4 position = lights.Load<float4>(at);
        const float4 direction = lights.Load<float4>(at + 16);
        const float4 radiance = lights.Load<float4>(at + 32);
        const float4 cone = lights.Load<float4>(at + 48);
        float3 to_light = -direction.xyz;
        float attenuation = 1.0;
        if (direction.w > 0.5)
        {
            const float3 offset = position.xyz - world;
            const float distance2 = max(dot(offset, offset), 1e-4);
            const float reach = distance2 * position.w;
            const float window = saturate(1.0 - reach * reach);
            to_light = offset * rsqrt(distance2);
            attenuation = window * window / distance2;
            if (direction.w > 1.5)
            {
                const float spot = saturate((dot(-to_light, direction.xyz) - cone.x) * cone.y);
                attenuation *= spot * spot;
            }
        }
        color += Shade(diffuse, specular, a2, normal, view, to_light, n_dot_v) *
                 radiance.rgb * attenuation;
    }
    const float occlusion = surface.b;
    const float3 ambient = SkyLight(normal, 1.0);
    float3 reflection = ambient;
    if ((Lighting.Lights.w & 1) != 0)
    {
        reflection = SkyLight(reflect(-view, normal), roughness);
    }
    color += (ambient * diffuse + reflection * EnvironmentBRDF(specular, roughness, n_dot_v)) * occlusion;
    return float4(color, 1);
}
