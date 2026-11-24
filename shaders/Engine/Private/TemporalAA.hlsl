// Each output pixel finds where its middle falls in the jittered frame,
// and takes the frame's sample nearest it, weighed by how near (a Gaussian
// fit of the Blackman-Harris window, in output pixels, as Unreal's TAAU
// weighs its samples): over a cycle of the jitter, each pixel gathers
// samples from all over its area, which smooths its edges and, from a frame
// rendered smaller, makes up the detail of the output's resolution. A frame
// without a history takes the frame filtered bilinearly at that place
// instead. Its place in the last frame comes from the nearest depth of
// the 3x3 texels around it (depth is reversed, so the greatest), through
// this frame's unjittered clip and the last's: the camera's motion alone,
// as nothing moves the renderer knows of. The history there is clamped
// to the box of the neighbourhood's colors in YCoCg (Karis, "High Quality
// Temporal Supersampling", 2014), which rejects what was not there, and
// blended with the frame by weights over one plus luma, which keeps bright
// pixels from flickering. A pixel whose last place was off the view, or a
// frame without a history, takes the frame alone.
struct TemporalIndices { uint Scene; uint Depth; uint History; uint Sampler; };
ConstantBuffer<TemporalIndices> Inputs : register(b0);
struct TemporalConstants
{
    float4x4 ClipToPrevious; float4 Jitter; float4 RenderSize; float4 OutputSize;
    float4 Blend;
};
ConstantBuffer<TemporalConstants> Temporal : register(b1);
struct VertexOut { float4 Position : SV_Position; };
VertexOut MainVS(uint vertex : SV_VertexID)
{
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    VertexOut output;
    output.Position = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}
float3 ToYCoCg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
float3 FromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}
float4 MainPS(VertexOut input) : SV_Target
{
    Texture2D<float4> scene = ResourceDescriptorHeap[Inputs.Scene];
    Texture2D<float> depth = ResourceDescriptorHeap[Inputs.Depth];
    Texture2D<float4> history = ResourceDescriptorHeap[Inputs.History];
    SamplerState filter = SamplerDescriptorHeap[Inputs.Sampler];
    const float2 uv = input.Position.xy * Temporal.OutputSize.zw;
    const float2 place = uv * Temporal.RenderSize.xy + Temporal.Jitter.xy;
    const float3 current =
        max(scene.SampleLevel(filter, place * Temporal.RenderSize.zw, 0).rgb, 0.0);
    if (Temporal.Blend.y == 0.0)
    {
        return float4(current, 1);
    }
    const int2 last = int2(Temporal.RenderSize.xy) - 1;
    const int2 middle = clamp(int2(floor(place)), int2(0, 0), last);
    const float2 offset = (place - (float2(middle) + 0.5)) * Temporal.Blend.z;
    const float nearness = exp(-2.29 * dot(offset, offset));
    const float3 sample_color = max(scene.Load(int3(middle, 0)).rgb, 0.0);
    float3 low = float3(1e30, 1e30, 1e30);
    float3 high = float3(-1e30, -1e30, -1e30);
    float nearest = 0.0;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            const int3 at = int3(clamp(middle + int2(x, y), int2(0, 0), last), 0);
            const float3 color = ToYCoCg(max(scene.Load(at).rgb, 0.0));
            low = min(low, color);
            high = max(high, color);
            nearest = max(nearest, depth.Load(at));
        }
    }
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 before = mul(Temporal.ClipToPrevious, float4(ndc, nearest, 1));
    const float2 before_ndc = before.xy / before.w;
    const float2 before_uv = float2(before_ndc.x * 0.5 + 0.5, 0.5 - before_ndc.y * 0.5);
    if (before.w <= 0.0 || before_uv.x < 0.0 || before_uv.y < 0.0 || before_uv.x > 1.0 ||
        before_uv.y > 1.0)
    {
        return float4(current, 1);
    }
    const float3 now = ToYCoCg(sample_color);
    const float3 past =
        clamp(ToYCoCg(max(history.SampleLevel(filter, before_uv, 0).rgb, 0.0)), low, high);
    const float weight_now = Temporal.Blend.x * nearness / (1.0 + now.x);
    const float weight_past = (1.0 - Temporal.Blend.x) / (1.0 + past.x);
    const float3 mixed = (now * weight_now + past * weight_past) / (weight_now + weight_past);
    return float4(max(FromYCoCg(mixed), 0.0), 1);
}
