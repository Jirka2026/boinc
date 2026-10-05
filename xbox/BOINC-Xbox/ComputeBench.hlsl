RWStructuredBuffer<uint> Output : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint x = id.x + 1u;
    [loop]
    for (uint i = 0; i < 256u; ++i)
    {
        x = x * 1664525u + 1013904223u;
    }
    Output[id.x] = x;
}
