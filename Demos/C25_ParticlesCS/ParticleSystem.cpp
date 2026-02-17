//***************************************************************************************
// ParticleSystem.cpp by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

#include "ParticleSystem.h"
#include "FrameResource.h"
#include "../../Common/DescriptorUtil.h"

using namespace DirectX;

ParticleSystem::ParticleSystem(ID3D12Device* device,
							   ResourceUploadBatch& uploadBatch,
							   uint32_t maxParticleCount,
							   bool requiresSorting) :
	md3dDevice(device),
	mMaxParticleCount(maxParticleCount),
	mRequiresSorting( requiresSorting )
{
	std::vector<uint32_t> indices(mMaxParticleCount * 6);
	for(uint32_t i = 0; i < mMaxParticleCount; ++i)
	{
		indices[i*6+0] = i * 4 + 0;
		indices[i*6+1] = i * 4 + 1;
		indices[i*6+2] = i * 4 + 2;

		indices[i*6+3] = i * 4 + 2;
		indices[i*6+4] = i * 4 + 1;
		indices[i*6+5] = i * 4 + 3;
	}

	CreateStaticBuffer(md3dDevice, uploadBatch,
					   indices.data(), indices.size(), sizeof(uint32_t),
					   D3D12_RESOURCE_STATE_INDEX_BUFFER, &mGeoIndexBuffer);

	BuildParticleBuffers(uploadBatch);

	mEmitInstances.reserve(16);
	mMemHandlesToEmitCB.reserve(16);
}

void ParticleSystem::Emit(const ParticleEmitCB& emitConstants)
{
	mEmitInstances.push_back(emitConstants);
}

void ParticleSystem::FrameSetup(const GameTimer& gt)
{
	mEmitInstances.clear();
	mMemHandlesToEmitCB.clear();
}

void ParticleSystem::Update(
	const GameTimer& gt,
	const XMFLOAT3& acceleration,
	ID3D12GraphicsCommandList* cmdList,
	ID3D12CommandSignature* updateParticlesCommandSig,
	ID3D12PipelineState* updateParticlesPso,
	ID3D12PipelineState* emitParticlesPso,
	ID3D12PipelineState* postUpdateParticlesPso,
	ID3D12Resource* particleCountReadback)
{
	GraphicsMemory& linearAllocator = GraphicsMemory::Get(md3dDevice);

	ParticleUpdateCB updateConstants;
	updateConstants.gAcceleration = acceleration;
	updateConstants.gParticleBufferUavIndex = mParticleBufferUavIndex;
	updateConstants.gFreeIndexBufferUavIndex = mFreeIndexBufferUavIndex;
	updateConstants.gPrevAliveIndexBufferUavIndex = mPrevAliveIndexBufferUavIndex;
	updateConstants.gCurrAliveIndexBufferUavIndex = mCurrAliveIndexBufferUavIndex;
	updateConstants.gFreeCountUavIndex = mFreeCountUavIndex;
	updateConstants.gPrevAliveCountUavIndex = mPrevAliveCountUavIndex;
	updateConstants.gCurrAliveCountUavIndex = mCurrAliveCountUavIndex;
	updateConstants.gIndirectArgsUavIndex = mIndirectArgsUavIndex;

	// Put bindless indices in our "extra" CB slot.
	mMemHandleUpdateCB = linearAllocator.AllocateConstant(updateConstants);
	cmdList->SetComputeRootConstantBufferView(
		COMPUTE_ROOT_ARG_PASS_EXTRA_CBV, 
		mMemHandleUpdateCB.GpuAddress());

	//
	// Update 
	//   Input: previous alive particle list.
	//   Output: particles still alive to currently alive list.
	//

	cmdList->SetPipelineState(updateParticlesPso);

	const uint32_t numCommands = 1;
	const uint32_t argOffset = 5 * sizeof(UINT);
	cmdList->ExecuteIndirect(
		updateParticlesCommandSig, 
		numCommands, 
		mIndirectArgsBuffer.Get(), 
		argOffset, 
		nullptr, 0);
	
	//
	// Append new particles to the currently alive list.
	//

	cmdList->SetPipelineState(emitParticlesPso);

	for(UINT i = 0; i < mEmitInstances.size(); ++i)
	{
		const ParticleEmitCB& emitConstants = mEmitInstances[i];

		// Need to hold handle until we submit work to GPU.
		GraphicsResource memHandle = linearAllocator.AllocateConstant(emitConstants);

		cmdList->SetComputeRootConstantBufferView(
			COMPUTE_ROOT_ARG_DISPATCH_CBV,
			memHandle.GpuAddress());

		UINT numGroupsX = (UINT)ceilf(emitConstants.gEmitCount / 128.0f);
		cmdList->Dispatch(numGroupsX, 1, 1);

		mMemHandlesToEmitCB.emplace_back(std::move(memHandle));
	}

	if(particleCountReadback != nullptr)
	{
		ScopedBarrier readbackBarrier(cmdList, 
		{ 
			CD3DX12_RESOURCE_BARRIER::Transition(
			mCurrAliveCountBuffer.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_SOURCE) 
		});

		cmdList->CopyResource(
			particleCountReadback,
			mCurrAliveCountBuffer.Get());
	}

	//
	// Post update CS
	//

	ScopedBarrier indirectArgsBarrier(cmdList, { 
		CD3DX12_RESOURCE_BARRIER::Transition(
		mIndirectArgsBuffer.Get(),
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS) });

	cmdList->SetComputeRootConstantBufferView(
		COMPUTE_ROOT_ARG_DISPATCH_CBV,
		mMemHandleUpdateCB.GpuAddress());
	cmdList->SetPipelineState(postUpdateParticlesPso);
	cmdList->Dispatch(1, 1, 1);

	
}

void ParticleSystem::Draw(
	ID3D12GraphicsCommandList* cmdList,
	ID3D12CommandSignature* drawParticlesCommandSig,
	ID3D12PipelineState* drawParticlesPso)
{
	cmdList->SetPipelineState(drawParticlesPso);

	D3D12_INDEX_BUFFER_VIEW ibv;
	ibv.BufferLocation = mGeoIndexBuffer->GetGPUVirtualAddress();
	ibv.Format = DXGI_FORMAT_R32_UINT;
	ibv.SizeInBytes = sizeof(std::uint32_t) * mMaxParticleCount * 6;

	cmdList->IASetVertexBuffers(0, 0, nullptr);
	cmdList->IASetIndexBuffer(&ibv);
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	ParticleDrawCB drawCB;
	drawCB.gParticleBufferIndex = GetParticleBufferUavIndex();
	drawCB.gParticleCurrAliveBufferIndex = GetCurrAliveIndexBufferUavIndex();

	GraphicsMemory& linearAllocator = GraphicsMemory::Get(md3dDevice);
	mMemHandleDrawCB = linearAllocator.AllocateConstant(drawCB);
	cmdList->SetGraphicsRootConstantBufferView(
		GFX_ROOT_ARG_OBJECT_CBV, 
		mMemHandleDrawCB.GpuAddress());

	// Draw the current particles.
	const uint32_t numCommands = 1;
	const uint32_t argOffset = 0;
	cmdList->ExecuteIndirect(
		drawParticlesCommandSig, 
		numCommands, 
		mIndirectArgsBuffer.Get(), 
		argOffset, 
		nullptr, 0);

	//
	// Swap: current becomes prev for next update.
	//

	std::swap(mPrevAliveIndexBufferUavIndex, mCurrAliveIndexBufferUavIndex);
	std::swap(mPrevAliveCountUavIndex, mCurrAliveCountUavIndex);
}

void ParticleSystem::BuildParticleBuffers(ResourceUploadBatch& uploadBatch)
{
	std::vector<Particle> zeroParticles(mMaxParticleCount, Particle());

	CreateStaticBuffer(md3dDevice, uploadBatch,
					   zeroParticles.data(), zeroParticles.size(), sizeof(Particle),
					   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mParticleBuffer.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	std::vector<uint32_t> zeroIndices(mMaxParticleCount, 0);

	CreateStaticBuffer(md3dDevice, uploadBatch,
					   zeroIndices.data(), zeroIndices.size(), sizeof(std::uint32_t),
					   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mPrevAliveIndexBuffer.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	CreateStaticBuffer(md3dDevice, uploadBatch,
					   zeroIndices.data(), zeroIndices.size(), sizeof(std::uint32_t),
					   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mCurrAliveIndexBuffer.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);


	// Put in reverse order because we grab the free indices at the end of the buffer (like a stack), and
	// we want the indices to start at the front of the particle buffer (i.e., start at index 0).
	std::vector<uint32_t> initFreeIndices(mMaxParticleCount);
	for(uint32_t i = 0; i < mMaxParticleCount; ++i)
		initFreeIndices[i] = mMaxParticleCount - 1 - i;

	CreateStaticBuffer(md3dDevice, uploadBatch,
					   initFreeIndices.data(), initFreeIndices.size(), sizeof(std::uint32_t),
					   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mFreeIndexBuffer.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	std::array<uint32_t, 1> initCounters;

	initCounters[0] = mMaxParticleCount;
	CreateStaticBuffer(md3dDevice, uploadBatch,
					   initCounters.data(), initCounters.size(), sizeof(std::uint32_t),
					   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mFreeCountBuffer.GetAddressOf(), 
					   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	initCounters[0] = 0;
	CreateStaticBuffer(md3dDevice, uploadBatch,
					   initCounters.data(), initCounters.size(), sizeof(std::uint32_t),
					   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mPrevAliveCountBuffer.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	initCounters[0] = 0;
	CreateStaticBuffer(md3dDevice, uploadBatch,
					   initCounters.data(), initCounters.size(), sizeof(std::uint32_t),
					   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mCurrAliveCountBuffer.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
 
	/*
	typedef struct D3D12_DRAW_INDEXED_ARGUMENTS
    {
    UINT IndexCountPerInstance;
    UINT InstanceCount;
    UINT StartIndexLocation;
    INT BaseVertexLocation;
    UINT StartInstanceLocation;
    } 	D3D12_DRAW_INDEXED_ARGUMENTS;
	*/
	/*
	typedef struct D3D12_DISPATCH_ARGUMENTS
	{
	UINT ThreadGroupCountX;
	UINT ThreadGroupCountY;
	UINT ThreadGroupCountZ;
	} 	D3D12_DISPATCH_ARGUMENTS;
	*/

	// Buffer stores args for 1 draw-indexed indirect and 1 dispatch indirect.
	// First 5 UINTs store D3D12_DRAW_ARGUMENTS, next 3 store D3D12_DISPATCH_ARGUMENTS.

	std::array<uint32_t, 8> initIndirect { 0, 0, 0, 0, 0, 0, 0, 0 };
	CreateStaticBuffer(md3dDevice, uploadBatch,
					   initIndirect.data(), initIndirect.size(), sizeof(std::uint32_t),
					   D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, mIndirectArgsBuffer.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
}

uint32_t ParticleSystem::GetParticleBufferUavIndex()const
{
	return mParticleBufferUavIndex;
}

uint32_t ParticleSystem::GetCurrAliveIndexBufferUavIndex()const
{
	return mCurrAliveIndexBufferUavIndex;
}

void ParticleSystem::BuildDescriptors()
{
	CbvSrvUavHeap& heap = CbvSrvUavHeap::Get();

	mParticleBufferUavIndex = heap.NextFreeIndex();
	mFreeIndexBufferUavIndex = heap.NextFreeIndex();
	mPrevAliveIndexBufferUavIndex = heap.NextFreeIndex();
	mCurrAliveIndexBufferUavIndex = heap.NextFreeIndex();
	mFreeCountUavIndex = heap.NextFreeIndex();
	mPrevAliveCountUavIndex = heap.NextFreeIndex();
	mCurrAliveCountUavIndex = heap.NextFreeIndex();
	mIndirectArgsUavIndex = heap.NextFreeIndex();

	CreateBufferUav(md3dDevice, 0, mMaxParticleCount, sizeof(Particle), 0, mParticleBuffer.Get(), nullptr, heap.CpuHandle(mParticleBufferUavIndex));
	CreateBufferUav(md3dDevice, 0, mMaxParticleCount, sizeof(uint32_t), 0, mFreeIndexBuffer.Get(), mFreeCountBuffer.Get(), heap.CpuHandle(mFreeIndexBufferUavIndex));
	CreateBufferUav(md3dDevice, 0, mMaxParticleCount, sizeof(uint32_t), 0, mPrevAliveIndexBuffer.Get(), mPrevAliveCountBuffer.Get(), heap.CpuHandle(mPrevAliveIndexBufferUavIndex));
	CreateBufferUav(md3dDevice, 0, mMaxParticleCount, sizeof(uint32_t), 0, mCurrAliveIndexBuffer.Get(), mCurrAliveCountBuffer.Get(), heap.CpuHandle(mCurrAliveIndexBufferUavIndex));

	CreateBufferUav(md3dDevice, 0, 1, sizeof(uint32_t), 0, mFreeCountBuffer.Get(), nullptr, heap.CpuHandle(mFreeCountUavIndex));
	CreateBufferUav(md3dDevice, 0, 1, sizeof(uint32_t), 0, mPrevAliveCountBuffer.Get(), nullptr, heap.CpuHandle(mPrevAliveCountUavIndex));
	CreateBufferUav(md3dDevice, 0, 1, sizeof(uint32_t), 0, mCurrAliveCountBuffer.Get(), nullptr, heap.CpuHandle(mCurrAliveCountUavIndex));

	CreateBufferUav(md3dDevice, 0, 8, sizeof(uint32_t), 0, mIndirectArgsBuffer.Get(), nullptr, heap.CpuHandle(mIndirectArgsUavIndex));
}