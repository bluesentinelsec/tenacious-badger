// A compute shader that writes each word of a buffer it reaches by its
// bindless index: the word's index times a value from a constant buffer.
struct DispatchConstants
{
    uint Buffer;
    uint Count;
};
ConstantBuffer<DispatchConstants> Dispatch : register(b0);

struct Factor
{
    uint Value;
};
ConstantBuffer<Factor> Scale : register(b1);

[numthreads(64, 1, 1)]
void MainCS(uint3 thread : SV_DispatchThreadID)
{
    RWByteAddressBuffer output = ResourceDescriptorHeap[Dispatch.Buffer];
    if (thread.x < Dispatch.Count)
    {
        output.Store(thread.x * 4, thread.x * Scale.Value);
    }
}
