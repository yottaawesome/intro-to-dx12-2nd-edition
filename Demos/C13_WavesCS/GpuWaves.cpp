//***************************************************************************************
// GpuWaves.cpp by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

#include "GpuWaves.h"
#include "FrameResource.h"
#include <algorithm>
#include <vector>
#include <cassert>

using namespace DirectX;

GpuWaves::GpuWaves(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch,
	               int m, int n, float dx, float dt, float speed, float damping)
{
	md3dDevice = device;

	mNumRows = m;
	mNumCols = n;

	assert((m*n) % 256 == 0);

	mVertexCount = m*n;
	mTriangleCount = (m - 1)*(n - 1) * 2;

	mTimeStep = dt;
	mSpatialStep = dx;

	float d = damping*dt + 2.0f;
	float e = (speed*speed)*(dt*dt) / (dx*dx);
	mK[0] = (damping*dt - 2.0f) / d;
	mK[1] = (4.0f - 8.0f*e) / d;
	mK[2] = (2.0f*e) / d;

	BuildResources(uploadBatch);
}

UINT GpuWaves::RowCount()const
{
	return mNumRows;
}

UINT GpuWaves::ColumnCount()const
{
	return mNumCols;
}

UINT GpuWaves::VertexCount()const
{
	return mVertexCount;
}

UINT GpuWaves::TriangleCount()const
{
	return mTriangleCount;
}

float GpuWaves::Width()const
{
	return mNumCols*mSpatialStep;
}

float GpuWaves::Depth()const
{
	return mNumRows*mSpatialStep;
}

float GpuWaves::SpatialStep()const
{
	return mSpatialStep;
}

uint32_t GpuWaves::DisplacementMapSrvIndex()const
{
	// It is mCurrSolSrvIndex here instead of mNextSolSrvIndex
	// because of the ping-pong done at the end of GpuWaves::Update().
	return mCurrSolSrvIndex;
}

void GpuWaves::SetConstants(float speed, float damping)
{
	float d = damping*mTimeStep + 2.0f;
	float e = (speed*speed)*(mTimeStep*mTimeStep) / (mSpatialStep*mSpatialStep);
	mK[0] = (damping*mTimeStep - 2.0f) / d;
	mK[1] = (4.0f - 8.0f*e) / d;
	mK[2] = (2.0f*e) / d;
}

void GpuWaves::BuildResources(DirectX::ResourceUploadBatch& uploadBatch)
{
	// All the textures for the wave simulation will be bound as a shader resource and
	// unordered access view at some point since we ping-pong the buffers.

	std::vector<float> zeroFloats(mVertexCount, 0.0f);

	D3D12_SUBRESOURCE_DATA initData;
	initData.pData = zeroFloats.data();
	initData.RowPitch = mNumCols * sizeof(float);
	initData.SlicePitch = initData.RowPitch * mNumRows;

	CreateTextureFromMemory(
		md3dDevice, uploadBatch,
		mNumCols, mNumRows,
		DXGI_FORMAT_R32_FLOAT,
		initData,
		mPrevSol.GetAddressOf(),
		false, // generateMips,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	CreateTextureFromMemory(
		md3dDevice, uploadBatch,
		mNumCols, mNumRows,
		DXGI_FORMAT_R32_FLOAT,
		initData,
		mCurrSol.GetAddressOf(),
		false, // generateMips,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	CreateTextureFromMemory(
		md3dDevice, uploadBatch,
		mNumCols, mNumRows,
		DXGI_FORMAT_R32_FLOAT,
		initData,
		mNextSol.GetAddressOf(),
		false, // generateMips,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	d3dSetDebugName(mPrevSol.Get(), "GpuWaves::mPrevSol");
	d3dSetDebugName(mCurrSol.Get(), "GpuWaves::mCurrSol");
	d3dSetDebugName(mNextSol.Get(), "GpuWaves::mNextSol");
}

void GpuWaves::BuildDescriptors()
{
	CbvSrvUavHeap& heap = CbvSrvUavHeap::Get();

	mPrevSolSrvIndex = heap.NextFreeIndex();
	mCurrSolSrvIndex = heap.NextFreeIndex();
	mNextSolSrvIndex = heap.NextFreeIndex();

	mPrevSolUavIndex = heap.NextFreeIndex();
	mCurrSolUavIndex = heap.NextFreeIndex();
	mNextSolUavIndex = heap.NextFreeIndex();

	const UINT mipLevels = 1;
	CreateSrv2d(md3dDevice, mPrevSol.Get(), DXGI_FORMAT_R32_FLOAT, mipLevels, heap.CpuHandle(mPrevSolSrvIndex));
	CreateSrv2d(md3dDevice, mCurrSol.Get(), DXGI_FORMAT_R32_FLOAT, mipLevels, heap.CpuHandle(mCurrSolSrvIndex));
	CreateSrv2d(md3dDevice, mNextSol.Get(), DXGI_FORMAT_R32_FLOAT, mipLevels, heap.CpuHandle(mNextSolSrvIndex));

	const UINT mipSlice = 0;
	CreateUav2d(md3dDevice, mPrevSol.Get(), DXGI_FORMAT_R32_FLOAT, mipSlice, heap.CpuHandle(mPrevSolUavIndex));
	CreateUav2d(md3dDevice, mCurrSol.Get(), DXGI_FORMAT_R32_FLOAT, mipSlice, heap.CpuHandle(mCurrSolUavIndex));
	CreateUav2d(md3dDevice, mNextSol.Get(), DXGI_FORMAT_R32_FLOAT, mipSlice, heap.CpuHandle(mNextSolUavIndex));
}

void GpuWaves::Update(
	const GameTimer& gt,
	ID3D12GraphicsCommandList* cmdList,
	ID3D12RootSignature* rootSig,
	ID3D12Resource* passCB,
	ID3D12PipelineState* pso)
{
	static float t = 0.0f;

	// Accumulate time.
	t += gt.DeltaTime();

	// Only update the simulation at the specified time step.
	if(t >= mTimeStep)
	{
		cmdList->SetPipelineState(pso);
		cmdList->SetComputeRootSignature(rootSig);

		cmdList->SetComputeRootConstantBufferView(
			COMPUTE_ROOT_ARG_PASS_CBV,
			passCB->GetGPUVirtualAddress());

		GpuWavesCB wavesCB;
		wavesCB.gWaveConstant0 = mK[0];
		wavesCB.gWaveConstant1 = mK[1];
		wavesCB.gWaveConstant2 = mK[2];
		wavesCB.gDisturbMag = 0.0f;
		wavesCB.gDisturbIndex = XMUINT2(0, 0);
		wavesCB.gGridSize = XMUINT2(mNumCols, mNumRows);
		wavesCB.gPrevSolIndex = mPrevSolUavIndex;
		wavesCB.gCurrSolIndex = mCurrSolUavIndex;
		wavesCB.gOutputIndex = mNextSolUavIndex;

		GraphicsMemory& linearAllocator = GraphicsMemory::Get(md3dDevice);
		mUpdateConstantsMemHandle = linearAllocator.AllocateConstant(wavesCB);
		cmdList->SetComputeRootConstantBufferView(
			COMPUTE_ROOT_ARG_DISPATCH_CBV,
			mUpdateConstantsMemHandle.GpuAddress());

		cmdList->ResourceBarrier(
			1, &CD3DX12_RESOURCE_BARRIER::Transition(
			mPrevSol.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_GENERIC_READ));

		cmdList->ResourceBarrier(
			1, &CD3DX12_RESOURCE_BARRIER::Transition(
			mCurrSol.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_GENERIC_READ));


		cmdList->ResourceBarrier(
			1, &CD3DX12_RESOURCE_BARRIER::Transition(
			mPrevSol.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		cmdList->ResourceBarrier(
			1, &CD3DX12_RESOURCE_BARRIER::Transition(
			mCurrSol.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		// How many groups do we need to dispatch to cover the wave grid.  
		// Note that mNumRows and mNumCols should be divisible by 16
		// so there is no remainder.
		UINT numGroupsX = mNumCols / 16;
		UINT numGroupsY = mNumRows / 16;
		cmdList->Dispatch(numGroupsX, numGroupsY, 1);

		//
		// Ping-pong buffers in preparation for the next update.
		// The previous solution is no longer needed and becomes the target of the next solution in the next update.
		// The current solution becomes the previous solution.
		// The next solution becomes the current solution.
		//

		auto resTemp = mPrevSol;
		mPrevSol = mCurrSol;
		mCurrSol = mNextSol;
		mNextSol = resTemp;

		auto srvTempIndex = mPrevSolSrvIndex;
		mPrevSolSrvIndex = mCurrSolSrvIndex;
		mCurrSolSrvIndex = mNextSolSrvIndex;
		mNextSolSrvIndex = srvTempIndex;

		auto uavTempIndex = mPrevSolUavIndex;
		mPrevSolUavIndex = mCurrSolUavIndex;
		mCurrSolUavIndex = mNextSolUavIndex;
		mNextSolUavIndex = uavTempIndex;

		t = 0.0f; // reset time
	}
}

void GpuWaves::Disturb(
	ID3D12GraphicsCommandList* cmdList,
	ID3D12RootSignature* rootSig,
	ID3D12Resource* passCB,
	ID3D12PipelineState* pso,
	UINT i, UINT j,
	float magnitude)
{
	cmdList->SetPipelineState(pso);
	cmdList->SetComputeRootSignature(rootSig);

	cmdList->SetComputeRootConstantBufferView(
		COMPUTE_ROOT_ARG_PASS_CBV,
		passCB->GetGPUVirtualAddress());

	GpuWavesCB wavesCB;
	wavesCB.gWaveConstant0 = mK[0];
	wavesCB.gWaveConstant1 = mK[1];
	wavesCB.gWaveConstant2 = mK[2];
	wavesCB.gDisturbMag = magnitude;
	wavesCB.gDisturbIndex = XMUINT2(j, i);
	wavesCB.gGridSize = XMUINT2(mNumCols, mNumRows);
	wavesCB.gPrevSolIndex = mPrevSolUavIndex;
	wavesCB.gCurrSolIndex = mCurrSolUavIndex;
	wavesCB.gOutputIndex = mNextSolUavIndex;

	GraphicsMemory& linearAllocator = GraphicsMemory::Get(md3dDevice);
	mDisturbConstantsMemHandle = linearAllocator.AllocateConstant(wavesCB);
	cmdList->SetComputeRootConstantBufferView(
		COMPUTE_ROOT_ARG_DISPATCH_CBV,
		mDisturbConstantsMemHandle.GpuAddress());

	// One thread group kicks off one thread, which displaces the height of one
	// vertex and its neighbors.
	cmdList->Dispatch(1, 1, 1);
}



 