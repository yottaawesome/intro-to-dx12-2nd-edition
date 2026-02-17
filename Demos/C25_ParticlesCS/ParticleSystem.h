//***************************************************************************************
// ParticleSystem.h by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/GameTimer.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Shaders/SharedTypes.h"


class ParticleSystem
{
public:
	ParticleSystem(ID3D12Device* device, 
				   DirectX::ResourceUploadBatch& uploadBatch,
				   uint32_t maxParticleCount,
				   bool requiresSorting);
		
	ParticleSystem(const ParticleSystem& rhs)=delete;
	ParticleSystem& operator=(const ParticleSystem& rhs)=delete;
	~ParticleSystem()=default;

	uint32_t GetParticleBufferUavIndex()const;
	uint32_t GetCurrAliveIndexBufferUavIndex()const;

	void BuildDescriptors();

	// Can call multiple times per frame to emit particles at different positions and with different properties.
	void Emit(const ParticleEmitCB& emitConstants);

	void FrameSetup(const GameTimer& gt);

	void Update(const GameTimer& gt, 
				const DirectX::XMFLOAT3& acceleration,
				ID3D12GraphicsCommandList* cmdList,
				ID3D12CommandSignature* updateParticlesCommandSig,
				ID3D12PipelineState* updateParticlesPso,
				ID3D12PipelineState* emitParticlesPso,
				ID3D12PipelineState* postUpdateParticlesPso,
				ID3D12Resource* particleCountReadback);

	void Draw(ID3D12GraphicsCommandList* cmdList,
			  ID3D12CommandSignature* drawParticlesCommandSig, 
			  ID3D12PipelineState* drawParticlesPso);

private:
	void BuildParticleBuffers(DirectX::ResourceUploadBatch& uploadBatch);

private:

	std::vector<ParticleEmitCB> mEmitInstances;

	std::vector<DirectX::GraphicsResource> mMemHandlesToEmitCB;
	DirectX::GraphicsResource mMemHandleUpdateCB;
	DirectX::GraphicsResource mMemHandleDrawCB;

	uint32_t mMaxParticleCount = 0;

	bool mRequiresSorting = false;

	uint32_t mParticleBufferUavIndex = -1;
	uint32_t mFreeIndexBufferUavIndex = -1;
	uint32_t mPrevAliveIndexBufferUavIndex = -1;
	uint32_t mCurrAliveIndexBufferUavIndex = -1;
	uint32_t mFreeCountUavIndex = -1;
	uint32_t mPrevAliveCountUavIndex = -1;
	uint32_t mCurrAliveCountUavIndex = -1;
	uint32_t mIndirectArgsUavIndex = -1;

	ID3D12Device* md3dDevice = nullptr;

	// For drawing.
	Microsoft::WRL::ComPtr<ID3D12Resource> mGeoIndexBuffer = nullptr;

	// Stores the actual particle data.
	Microsoft::WRL::ComPtr<ID3D12Resource> mParticleBuffer = nullptr;

	// Stores indices to free particles.
	Microsoft::WRL::ComPtr<ID3D12Resource> mFreeIndexBuffer = nullptr;

	// Stores indices to previous alive particles.
	Microsoft::WRL::ComPtr<ID3D12Resource> mPrevAliveIndexBuffer = nullptr;

	// Stores indices of alive particles to draw. During update, we kill off
	// particles, so need a new updated list of alive particles to draw.
	Microsoft::WRL::ComPtr<ID3D12Resource> mCurrAliveIndexBuffer = nullptr;

	Microsoft::WRL::ComPtr<ID3D12Resource> mFreeCountBuffer = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mPrevAliveCountBuffer = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mCurrAliveCountBuffer = nullptr;

	Microsoft::WRL::ComPtr<ID3D12Resource> mIndirectArgsBuffer = nullptr;
};

 