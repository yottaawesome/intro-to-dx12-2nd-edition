
#include "Shaders/Common.hlsl"

//
// Every frame
//   *Clear counter for gCurrAliveIndexBuffer to 0 as we rebuild the list every
//    frame so that we have a contiguous list of alive particles to draw.
//   *Update prev particles and append alive particles to gCurrAliveIndexBuffer.
//   *Emit new particles to gCurrAliveIndexBuffer.
//   *Draw gCurrAliveCount particles
//   *Set gPrevAliveCount = 0 to prepare for next frame This is effectively clearing 
//    the counter for gCurrAliveIndexBuffer in the next frame because of the following swap.
//    We clear it because in the update and emit we rebuild the current list so that we have
//    a contiguous list of alive particles to draw.
//   *Swap for next frame: 
//     swap(gPrevAliveIndexBuffer, gCurrAliveIndexBuffer)
//     swap(gPrevAliveCount, gCurrAliveCount)
//

 
void UpdateParticle(inout Particle p)
{
	// Simulate a drag effect where the particle slows down over time. 
	// This effect can be controlled with the gDragScale constant.
	float speedSquared = dot(p.Velocity, p.Velocity);

	float3 drag = float3(0.0f, 0.0f, 0.0f);
	if(speedSquared > 0.001f)
	{
		drag = -p.DragScale * speedSquared * normalize(p.Velocity);
	}

	// Add in global acceleration due to wind and gravity.
	float3 acceleration = drag + gAcceleration;

	p.Position += p.Velocity * gDeltaTime;
	p.Velocity += acceleration * gDeltaTime;
	p.Rotation += p.RotationSpeed * gDeltaTime;
	p.Age += gDeltaTime;
}

[numthreads(128, 1, 1)]
void ParticlesUpdateCS(uint3 groupThreadID : SV_GroupThreadID,
					   uint3 dispatchThreadID : SV_DispatchThreadID)
{
	RWStructuredBuffer<Particle> particleBuffer   = ResourceDescriptorHeap[gParticleBufferUavIndex];
	RWStructuredBuffer<uint> freeIndexBuffer      = ResourceDescriptorHeap[gFreeIndexBufferUavIndex];
	RWStructuredBuffer<uint> currAliveIndexBuffer = ResourceDescriptorHeap[gCurrAliveIndexBufferUavIndex];
	RWStructuredBuffer<uint> prevAliveIndexBuffer = ResourceDescriptorHeap[gPrevAliveIndexBufferUavIndex];
	RWStructuredBuffer<uint> prevAliveCountBuffer = ResourceDescriptorHeap[gPrevAliveCountUavIndex];

	if(dispatchThreadID.x < prevAliveCountBuffer[0])
	{
		uint particleIndex = prevAliveIndexBuffer[dispatchThreadID.x];

		UpdateParticle(particleBuffer[particleIndex]);

		if(particleBuffer[particleIndex].Age >= particleBuffer[particleIndex].Lifetime)
		{
			// Particle died, append to free list.
			uint oldIndex = freeIndexBuffer.IncrementCounter();
			freeIndexBuffer[oldIndex] = particleIndex;
		}
		else
		{
			// Particle is still alive, append to alive list.
			uint oldIndex = currAliveIndexBuffer.IncrementCounter();
			currAliveIndexBuffer[oldIndex] = particleIndex;
		}
	}
}

// Remap [0,1) -> [a, b)
float Remap(float value, float a, float b)
{
	float range = b - a;
	return range * value + a;
}

void InitParticle(uint index, inout Particle p)
{
	Texture2D randVecMap = ResourceDescriptorHeap[gRandomTexIndex];

	uint randTexWidth;
	uint randTexHeight;
	randVecMap.GetDimensions(randTexWidth, randTexHeight);

	// Random per index.
	float randOffset = index / float(randTexWidth);

	float2 tex0 = gEmitRandomValues.xy + randOffset;
	float2 tex1 = gEmitRandomValues.zw + randOffset;
	float2 tex2 = (1.0f - gEmitRandomValues.xy) + randOffset;

	float4 rand0 = randVecMap.SampleLevel(GetLinearWrapSampler(), tex0, 0.0f).rgba;
	float4 rand1 = randVecMap.SampleLevel(GetLinearWrapSampler(), tex1, 0.0f).rgba;
	float4 rand2 = randVecMap.SampleLevel(GetLinearWrapSampler(), tex2, 0.0f).rgba;

	float3 initialPosition;
	initialPosition.x = Remap(rand0.x, gEmitBoxMin.x, gEmitBoxMax.x);
	initialPosition.y = Remap(rand0.y, gEmitBoxMin.y, gEmitBoxMax.y);
	initialPosition.z = Remap(rand0.z, gEmitBoxMin.z, gEmitBoxMax.z);
	
	float initialSpeed = Remap(rand0.w, gMinInitialSpeed, gMaxInitialSpeed);

	float3 direction;
	direction.x = Remap(rand1.x, gEmitDirectionMin.x, gEmitDirectionMax.x);
	direction.y = Remap(rand1.y, gEmitDirectionMin.y, gEmitDirectionMax.y);
	direction.z = Remap(rand1.z, gEmitDirectionMin.z, gEmitDirectionMax.z);

	direction = normalize(direction);

	float3 initialVelocity = initialSpeed * direction;

	float4 color = lerp(gEmitColorMin, gEmitColorMax, rand0);

	p.Position = initialPosition;
	p.Velocity = initialVelocity;
	p.Color = color;
	p.Lifetime = Remap(rand1.w, gMinLifetime, gMaxLifetime);
	p.Age = 0.0f;
	p.Size.x = Remap(rand2.x, gMinScale.x, gMaxScale.x);
	p.Size.y = Remap(rand2.y, gMinScale.y, gMaxScale.y);
	p.Rotation = Remap(rand2.z, gMinRotation, gMaxRotation);
	p.RotationSpeed = Remap(rand2.w, gMinRotationSpeed, gMaxRotationSpeed);
	p.DragScale = gDragScale;
	p.BindlessTextureIndex = gBindlessTextureIndex;
}

[numthreads(128, 1, 1)]
void ParticlesEmitCS(uint3 groupThreadID : SV_GroupThreadID,
					 uint3 dispatchThreadID : SV_DispatchThreadID)
{
	RWStructuredBuffer<Particle> particleBuffer   = ResourceDescriptorHeap[gParticleBufferUavIndex];
	RWStructuredBuffer<uint> freeIndexBuffer      = ResourceDescriptorHeap[gFreeIndexBufferUavIndex];
	RWStructuredBuffer<uint> freeCountBuffer      = ResourceDescriptorHeap[gFreeCountUavIndex];
	RWStructuredBuffer<uint> currAliveIndexBuffer = ResourceDescriptorHeap[gCurrAliveIndexBufferUavIndex];

	// Can only emit particles that we have space for.
	uint emitCount = min(gEmitCount, freeCountBuffer[0]);

	if(dispatchThreadID.x < emitCount)
	{
		uint freeIndex = freeIndexBuffer.DecrementCounter();
		uint particleIndex = freeIndexBuffer[freeIndex];

		InitParticle(dispatchThreadID.x, particleBuffer[particleIndex]);

		// Append particle index to alive list
		uint oldIndex = currAliveIndexBuffer.IncrementCounter();
		currAliveIndexBuffer[oldIndex] = particleIndex;
	}
}

[numthreads(1, 1, 1)]
void PostUpdateCS(uint3 groupThreadID : SV_GroupThreadID,
				  uint3 dispatchThreadID : SV_DispatchThreadID)
{
	RWStructuredBuffer<uint> prevAliveCountBuffer = ResourceDescriptorHeap[gPrevAliveCountUavIndex];
	RWStructuredBuffer<uint> currAliveCountBuffer = ResourceDescriptorHeap[gCurrAliveCountUavIndex];

	RWStructuredBuffer<uint> indirectArgsBuffer   = ResourceDescriptorHeap[gIndirectArgsUavIndex];

	// This is effectively clearing counter for gCurrAliveIndexBuffer in the next frame
	// because after drawing, we swap(prevAlive, currAlive) buffers.
	prevAliveCountBuffer[0] = 0;

	// Update draw indirect args.
	indirectArgsBuffer[0] = currAliveCountBuffer[0] * 6;
	indirectArgsBuffer[1] = 1;
	indirectArgsBuffer[2] = 0;
	indirectArgsBuffer[3] = 0;
	indirectArgsBuffer[4] = 0;

	// Update dispatch indirect args.
	uint numThreadGroupsX = (currAliveCountBuffer[0] + 127) / 128;
	indirectArgsBuffer[5] = numThreadGroupsX;
	indirectArgsBuffer[6] = 1;
	indirectArgsBuffer[7] = 1;
}
