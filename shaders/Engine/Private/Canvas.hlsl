// Each quad's pixels are corner + u * axis_x + v * axis_y, grown by a pixel
// on every side when its edges are smoothed; the pixel shader measures the
// distance to its rounded box's edge in pixels, and keeps the pixels inside
// its clip. Colors and images are premultiplied. A quad of text reads its
// image's first channel as coverage, which multiplies its color.
struct CanvasConstants { float2 Target; uint Quads; uint Offset; };
ConstantBuffer<CanvasConstants> Canvas : register(b0);
struct VertexOut
{
    float4 Position : SV_Position;
    float2 Local : TEXCOORD0;
    float2 UV : TEXCOORD1;
    float4 Color : COLOR0;
    nointerpolation float4 Clip : TEXCOORD2;
    nointerpolation float4 Shape : TEXCOORD3;
    nointerpolation uint3 Binding : TEXCOORD4;
};
VertexOut MainVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    ByteAddressBuffer quads = ResourceDescriptorHeap[Canvas.Quads];
    const uint base = Canvas.Offset + instance * 96;
    const float4 geometry = quads.Load<float4>(base);
    const float4 shape = quads.Load<float4>(base + 16);
    const float4 uv = quads.Load<float4>(base + 32);
    const float2 size = float2(length(geometry.zw), length(shape.xy));
    const uint3 binding = quads.Load<uint4>(base + 80).xyz;
    const float2 corner = float2((0x1A >> vertex) & 1, (0x34 >> vertex) & 1);
    const float grow = (binding.z & 1) != 0 ? 1.0 : 0.0;
    const float2 local = corner * size + (corner * 2.0 - 1.0) * grow;
    const float2 along = local / max(size, 0.000001);
    const float2 pixel = geometry.xy + along.x * geometry.zw + along.y * shape.xy;
    VertexOut output;
    output.Position = float4(pixel.x / Canvas.Target.x * 2.0 - 1.0,
                             1.0 - pixel.y / Canvas.Target.y * 2.0, 0.0, 1.0);
    output.Local = local;
    output.UV = lerp(uv.xy, uv.zw, along);
    output.Color = quads.Load<float4>(base + 48);
    output.Clip = quads.Load<float4>(base + 64);
    output.Shape = float4(size, shape.zw);
    output.Binding = binding;
    return output;
}
float RoundedBox(float2 p, float2 half_size, float radius)
{
    const float2 q = abs(p) - half_size + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}
float4 MainPS(VertexOut input) : SV_Target
{
    Texture2D<float4> image = ResourceDescriptorHeap[NonUniformResourceIndex(input.Binding.x)];
    SamplerState filter = SamplerDescriptorHeap[NonUniformResourceIndex(input.Binding.y)];
    const float4 texel = image.Sample(filter, input.UV);
    const float4 color =
        (input.Binding.z & 2) != 0 ? input.Color * texel.r : texel * input.Color;
    float coverage = 1.0;
    if ((input.Binding.z & 1) != 0)
    {
        const float2 half_size = input.Shape.xy * 0.5;
        const float radius = min(input.Shape.z, min(half_size.x, half_size.y));
        float edge = RoundedBox(input.Local - half_size, half_size, radius);
        if (input.Shape.w > 0.0)
        {
            edge = abs(edge + input.Shape.w * 0.5) - input.Shape.w * 0.5;
        }
        coverage = saturate(0.5 - edge);
    }
    const float2 position = input.Position.xy;
    const bool inside = all(position >= input.Clip.xy) && all(position < input.Clip.zw);
    return color * (inside ? coverage : 0.0);
}
