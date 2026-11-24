// The sky's triangle covers the view, at depth 0: the far plane, as depth
// is reversed. Each pixel's direction is the camera's forward, plus its
// place in the view times the edges' directions. Above the horizon the
// color runs from the horizon's to the zenith's, quickly near the horizon
// as a sky's does; below, to the ground's over a few degrees. The sun's
// disc is smoothed over a pixel, with a faint glow around it.
struct SkyConstants
{
    float4 Forward; float4 Right; float4 Up; float4 Zenith; float4 Horizon; float4 Ground;
    float4 Sun; float4 SunColor; uint4 Cube;
};
ConstantBuffer<SkyConstants> Sky : register(b1);
struct VertexOut { float4 Position : SV_Position; float2 View : TEXCOORD0; };
VertexOut MainVS(uint vertex : SV_VertexID)
{
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    const float2 view = corner * float2(2, -2) + float2(-1, 1);
    VertexOut output;
    output.Position = float4(view, 0, 1);
    output.View = view;
    return output;
}
float3 Procedural(float3 direction)
{
    float3 color = lerp(Sky.Horizon.rgb, Sky.Ground.rgb, saturate(-direction.y * 12.0));
    if (direction.y >= 0.0)
    {
        color = lerp(Sky.Horizon.rgb, Sky.Zenith.rgb, sqrt(direction.y));
    }
    const float facing = dot(direction, Sky.Sun.xyz);
    const float edge = max(fwidth(facing), 1e-6);
    const float disc = smoothstep(Sky.Sun.w - edge, Sky.Sun.w + edge, facing);
    const float glow = pow(saturate(facing), 64.0) * 0.25;
    const float sun = Sky.SunColor.w;
    color = lerp(color, Sky.SunColor.rgb, disc * sun) + Sky.SunColor.rgb * glow * sun;
    return color;
}
float4 MainPS(VertexOut input) : SV_Target
{
    const float3 direction = normalize(Sky.Forward.xyz + input.View.x * Sky.Right.xyz +
                                       input.View.y * Sky.Up.xyz);
    if (Sky.Cube.x != 0)
    {
        TextureCube<float4> cube = ResourceDescriptorHeap[Sky.Cube.y];
        SamplerState filter = SamplerDescriptorHeap[Sky.Cube.z];
        return float4(cube.Sample(filter, direction).rgb, 1);
    }
    return float4(Procedural(direction), 1);
}
