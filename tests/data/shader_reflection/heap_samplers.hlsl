// dxc: -T ps_6_6 -E main
// Samplers taken from the sampler heap, and one that is bound, sampling a
// bound texture: the resource heap is not used, so DXC names only the
// sampler heap.
Texture2D<float> shadow : register(t2);
SamplerState bound : register(s1);

cbuffer Frame
{
    uint linear_index;
    uint compare_index;
};

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    SamplerState smooth = SamplerDescriptorHeap[linear_index];
    SamplerComparisonState compare = SamplerDescriptorHeap[compare_index];
    float lit = shadow.SampleCmpLevelZero(compare, uv, 0.5);
    return float4(lit, shadow.Sample(smooth, uv), shadow.Sample(bound, uv), 1);
}
