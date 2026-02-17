
// Include common HLSL code.
#include "Shaders/Common.hlsl"

// UpdateWavesCS(): Solves 2D wave equation using the compute shader.
[numthreads(16, 16, 1)]
void UpdateWavesCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	// We do not need to do bounds checking because:
	//	 *out-of-bounds reads return 0, which works for us--it just means the boundary of 
	//    our water simulation is clamped to 0 in local space.
	//   *out-of-bounds writes are a no-op.
	
	int x = dispatchThreadID.x;
	int y = dispatchThreadID.y;
	
	RWTexture2D<float> gPrevSolInput = ResourceDescriptorHeap[gPrevSolIndex];
	RWTexture2D<float> gCurrSolInput = ResourceDescriptorHeap[gCurrSolIndex];
	RWTexture2D<float> gOutput       = ResourceDescriptorHeap[gOutputIndex];
	
	gOutput[uint2(x,y)] = 
		gWaveConstant0 * gPrevSolInput[int2(x,y)].r +
		gWaveConstant1 * gCurrSolInput[int2(x,y)].r +
		gWaveConstant2 * (
			gCurrSolInput[uint2(x,y+1)].r + 
			gCurrSolInput[uint2(x,y-1)].r + 
			gCurrSolInput[uint2(x+1,y)].r + 
			gCurrSolInput[uint2(x-1,y)].r);
			
}

// DisturbWavesCS(): Runs one thread to disturb a grid height and its
// neighbors to generate a wave. 
[numthreads(1, 1, 1)]
void DisturbWavesCS(int3 groupThreadID : SV_GroupThreadID,
                    int3 dispatchThreadID : SV_DispatchThreadID)
{
	// We do not need to do bounds checking because:
	//	 *out-of-bounds reads return 0, which works for us--it just means the boundary of 
	//    our water simulation is clamped to 0 in local space.
	//   *out-of-bounds writes are a no-op.
	
	int x = gDisturbIndex.x;
	int y = gDisturbIndex.y;

	int gridWidth = gGridSize.x;

	RWTexture2D<float> gOutput = ResourceDescriptorHeap[gCurrSolIndex];

	float halfMag = 0.5f*gDisturbMag;

	// Buffer is RW so operator += is well defined.
	gOutput[int2(x,y)]   += gDisturbMag;
	gOutput[int2(x+1,y)] += halfMag;
	gOutput[int2(x-1,y)] += halfMag;
	gOutput[int2(x,y+1)] += halfMag;
	gOutput[int2(x,y-1)] += halfMag;
}
