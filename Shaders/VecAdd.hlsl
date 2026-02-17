
struct Data
{
	float3 v1;
	float2 v2;
};

cbuffer DispatchCB : register(b0)
{
    uint gBufferIndexA;
    uint gBufferIndexB;
    uint gBufferIndexOutput;
    uint DispatchCB_Pad0;
};


[numthreads(32, 1, 1)]
void CS(int3 dtid : SV_DispatchThreadID)
{
	StructuredBuffer<Data> gInputA = ResourceDescriptorHeap[gBufferIndexA];
	StructuredBuffer<Data> gInputB = ResourceDescriptorHeap[gBufferIndexB];
	RWStructuredBuffer<Data> gOutput = ResourceDescriptorHeap[gBufferIndexOutput];

	gOutput[dtid.x].v1 = gInputA[dtid.x].v1 + gInputB[dtid.x].v1;
	gOutput[dtid.x].v2 = gInputA[dtid.x].v2 + gInputB[dtid.x].v2;
}
