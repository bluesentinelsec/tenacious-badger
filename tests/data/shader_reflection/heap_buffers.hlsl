// dxc: -T ps_6_6 -E main
// Every kind of read-only buffer taken from the resource heap, beside one
// that is bound: only the bound one has a binding.
struct Particle
{
    float3 position;
    uint flags;
};

Buffer<float4> bound;

float4 main(nointerpolation uint index : INDEX) : SV_Target
{
    Buffer<float4> typed = ResourceDescriptorHeap[index];
    ByteAddressBuffer raw = ResourceDescriptorHeap[index + 1];
    StructuredBuffer<Particle> particles = ResourceDescriptorHeap[index + 2];
    float4 total = typed[index] + bound[index];
    total.xy += asfloat(raw.Load2(index * 8));
    total.xyz += particles[index].position;
    total.w += (float)particles.Load(index + 1).flags;
    return total;
}
