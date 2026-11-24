// dxc: -T cs_6_6 -E main
// A compute shader that writes what it reads, all through the resource
// heap: a writable byte-address buffer, structured buffer and texture.
cbuffer Indices : register(b0)
{
    uint source_index;
    uint target_index;
    uint image_index;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    StructuredBuffer<float> source = ResourceDescriptorHeap[source_index];
    RWByteAddressBuffer target = ResourceDescriptorHeap[target_index];
    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[target_index + 1];
    RWTexture2D<float4> image = ResourceDescriptorHeap[image_index];
    const float value = source[id.x];
    target.Store(id.x * 4, asuint(value * 2));
    InterlockedAdd(counts[0], 1);
    image[id.xy] = float4(value, 0, 0, 1);
}
