//***************************************************************************************
// RayTracer.h by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Common/UploadBuffer.h"
#include "DirectXRaytracingHelper.h"

class ProceduralRayTracer
{
	struct RTInstance
	{
		DirectX::XMFLOAT4X4 Transform;
		uint32_t MaterialIndex = 0;
		uint32_t PrimitiveType = 0;
		DirectX::XMFLOAT2 TexScale = { 1.0f, 1.0f };
	};

public:

	static constexpr wchar_t* HitGroupName = L"HitGroup0";
	static constexpr wchar_t* RaygenShaderName = L"RaygenShader";
	static constexpr wchar_t* ClosestHitShaderName = L"ClosestHit";
	static constexpr wchar_t* ColorMissShaderName = L"Color_MissShader";
	static constexpr wchar_t* ShadowMissShaderName = L"Shadow_MissShader";
	static constexpr wchar_t* IntersectionShaderName = L"PrimitiveIntersectionShader";

	ProceduralRayTracer(ID3D12Device5* device, 
						ID3D12GraphicsCommandList6* cmdList,
						IDxcBlob* rayTraceLibByteCode,
						DXGI_FORMAT format, UINT width, UINT height);
		
	ProceduralRayTracer(const ProceduralRayTracer& rhs)=delete;
	ProceduralRayTracer& operator=(const ProceduralRayTracer& rhs)=delete;
	~ProceduralRayTracer()=default;

	ID3D12Resource* GetOutputImage()const;
	UINT GetOutputTextureUavIndex()const;
	UINT GetOutputTextureSrvIndex()const;

	void OnResize(UINT newWidth, UINT newHeight);

	// The box is [-1, 1]^3 in local space.
	void AddBox(const DirectX::XMFLOAT4X4& worldTransform, DirectX::XMFLOAT2 texScale, UINT materialIndex);

	// The cylinder is centered at the origin, aligned with +y axis, has radius 1 and length 2 in local space.
	void AddCylinder( const DirectX::XMFLOAT4X4& worldTransform, DirectX::XMFLOAT2 texScale, UINT materialIndex);

	// The disk is centered at the origin, with normal aimed down the +y-axis, and has radius 1 in local space.
	void AddDisk(const DirectX::XMFLOAT4X4& worldTransform, DirectX::XMFLOAT2 texScale, UINT materialIndex);

	// The sphere is centered at origin with radius 1 in local space.
	void AddSphere(const DirectX::XMFLOAT4X4& worldTransform, DirectX::XMFLOAT2 texScale, UINT materialIndex);

	// Cannot add anymore geometries once we start building.
	void ExecuteBuildAccelerationStructureCommands(ID3D12CommandQueue* commandQueue);

	void Draw(ID3D12Resource* passCB, ID3D12Resource* matBuffer);

private:
	void BuildOutputTexture();
	void BuildGlobalRootSignature();
	void BuildLocalRootSignature();
	void BuildRayTraceStateObject();

	AccelerationStructureBuffers BuildPrimitiveBlas();
	Microsoft::WRL::ComPtr<ID3D12Resource> BuildInstanceBuffer(D3D12_GPU_VIRTUAL_ADDRESS blasGpuAddress);
	AccelerationStructureBuffers BuildTlas(D3D12_GPU_VIRTUAL_ADDRESS blasGpuAddress);
	void BuildShaderBindingTables();
	void BuildDescriptors();

private:

	static constexpr uint32_t RayCount = 2; // primary / shadow
	static constexpr uint32_t NumGeometriesPerInstance = 1;

	ID3D12Device5* mdxrDevice = nullptr;
	ID3D12GraphicsCommandList6* mdxrCmdList = nullptr;
	Microsoft::WRL::ComPtr<ID3D12StateObject> mdxrStateObject = nullptr;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> mGlobalRootSig = nullptr;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> mLocalRootSig = nullptr;

	D3D12_SHADER_BYTECODE mShaderLib;

	UINT mWidth = 0;
	UINT mHeight = 0;
	DXGI_FORMAT mFormat = DXGI_FORMAT_UNKNOWN;

	UINT mOutputTextureUavIndex = -1;
	UINT mOutputTextureSrvIndex = -1;

	Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mPrimitiveBlas = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mSceneTlas = nullptr;

	std::vector<RTInstance> mInstances;

	std::vector<D3D12_RAYTRACING_AABB> mInstanceBounds;
	std::unique_ptr<UploadBuffer<D3D12_RAYTRACING_AABB>> mGeoBoundsBuffer = nullptr;

	// shader table
	Microsoft::WRL::ComPtr<ID3D12Resource> mMissShaderTable;
	Microsoft::WRL::ComPtr<ID3D12Resource> mHitGroupShaderTable;
	Microsoft::WRL::ComPtr<ID3D12Resource> mRayGenShaderTable;

	UINT64 mHitGroupShaderTableStrideInBytes = 0;
	UINT64 mMissShaderTableStrideInBytes = 0;
};

 