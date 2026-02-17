//***************************************************************************************
// GpuWaves.h by Frank Luna (C) 2011 All Rights Reserved.
//
// Performs the calculations for the wave simulation using the ComputeShader on the GPU.  
// The solution is saved to a floating-point texture.  The client must then set this 
// texture as a SRV and do the displacement mapping in the vertex shader over a grid.
//***************************************************************************************

#ifndef GPUWAVES_H
#define GPUWAVES_H

#include "../../Common/d3dUtil.h"
#include "../../Common/GameTimer.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Shaders/SharedTypes.h"

class GpuWaves
{
public:
	// Note that m,n should be divisible by 16 so there is no 
	// remainder when we divide into thread groups.
	GpuWaves(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, int m, int n, float dx, float dt, float speed, float damping);
	GpuWaves(const GpuWaves& rhs) = delete;
	GpuWaves& operator=(const GpuWaves& rhs) = delete;
	~GpuWaves()=default;

	UINT RowCount()const;
	UINT ColumnCount()const;
	UINT VertexCount()const;
	UINT TriangleCount()const;
	float Width()const;
	float Depth()const;
	float SpatialStep()const;

	uint32_t DisplacementMapSrvIndex()const;

	void SetConstants(float speed, float damping);

	void BuildResources(DirectX::ResourceUploadBatch& uploadBatch);
	void BuildDescriptors();

	void Update(
		const GameTimer& gt,
		ID3D12GraphicsCommandList* cmdList, 
		ID3D12RootSignature* rootSig,
		ID3D12Resource* passCB,
		ID3D12PipelineState* pso);

	void Disturb(
		ID3D12GraphicsCommandList* cmdList,
		ID3D12RootSignature* rootSig,
		ID3D12Resource* passCB,
		ID3D12PipelineState* pso, 
		UINT i, UINT j, 
		float magnitude);

private:

	UINT mNumRows;
	UINT mNumCols;

	UINT mVertexCount;
	UINT mTriangleCount;

	// Simulation constants we can precompute.
	float mK[3];

	float mTimeStep;
	float mSpatialStep;

	ID3D12Device* md3dDevice = nullptr;

	DirectX::GraphicsResource mUpdateConstantsMemHandle;
	DirectX::GraphicsResource mDisturbConstantsMemHandle;

	// For rendering we just need to read from the resource, so a 
	// SRV is likely more performant for readonly. It also means we
	// can sample the texture if we needed to.
	uint32_t mPrevSolSrvIndex = -1;
	uint32_t mCurrSolSrvIndex = -1;
	uint32_t mNextSolSrvIndex = -1;

	uint32_t mPrevSolUavIndex = -1;
	uint32_t mCurrSolUavIndex = -1;
	uint32_t mNextSolUavIndex = -1;

	// Two for ping-ponging the textures.
	Microsoft::WRL::ComPtr<ID3D12Resource> mPrevSol = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mCurrSol = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mNextSol = nullptr;
};

#endif // GPUWAVES_H