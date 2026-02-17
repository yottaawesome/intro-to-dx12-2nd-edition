//***************************************************************************************
// HybridRayTracer.h by Frank Luna (C) 2023 All Rights Reserved.
//***************************************************************************************

#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Common/UploadBuffer.h"
#include "DirectXRaytracingHelper.h"

class HybridRayTracer
{

	struct RTInstance
	{
		std::string ModelName;
		DirectX::XMFLOAT4X4 Transform;
		uint32_t MaterialIndex;
		DirectX::XMFLOAT2 TexScale;
	};
	
public:

	static const DXGI_FORMAT ReflectionMapFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	// In our demo, we have one geometry per model. However, a model could be built from multiple geometries.
	// For example, a car model could be made from different geometries for the wheels, body, seats, 
	// windows, etc. 
	struct RTModelDef
	{
		ID3D12Resource* VertexBuffer = nullptr;
		ID3D12Resource* IndexBuffer = nullptr;
		UINT VertexBufferBindlessIndex = -1;
		UINT IndexBufferBindlessIndex = -1;
		DXGI_FORMAT IndexFormat = DXGI_FORMAT_R16_UINT;
		DXGI_FORMAT VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		UINT IndexCount = 0;
		UINT VertexCount = 0;
		UINT StartIndexLocation = 0;
		UINT BaseVertexLocation = 0;
		UINT VertexSizeInBytes = 0;
		UINT IndexSizeInBytes = 2;
	};

	static constexpr wchar_t* HitGroupName = L"HitGroup0";
	static constexpr wchar_t* RaygenShaderName = L"RaygenShader";
	static constexpr wchar_t* ClosestHitShaderName = L"ClosestHit";
	static constexpr wchar_t* ColorMissShaderName = L"Color_MissShader";
	static constexpr wchar_t* ShadowMissShaderName = L"Shadow_MissShader";

	HybridRayTracer(ID3D12Device5* device, 
						ID3D12GraphicsCommandList6* cmdList,
						IDxcBlob* rayTraceLibByteCode,
						UINT width, UINT height);
		
	HybridRayTracer(const HybridRayTracer& rhs)=delete;
	HybridRayTracer& operator=(const HybridRayTracer& rhs)=delete;
	~HybridRayTracer()=default;

	ID3D12Resource* GetReflectionMap()const;
	UINT GetReflectionMapUavIndex()const;
	UINT GetReflectionMapSrvIndex()const;

	void OnResize(UINT newWidth, UINT newHeight);

	void AddModel(const std::string& modelName, const RTModelDef& modelDef);
	void AddInstance(const std::string& modelName, 
					 const DirectX::XMFLOAT4X4& worldTransform, 
					 DirectX::XMFLOAT2 texScale,
					 UINT materialIndex);

	// Cannot add anymore geometries once we start building.
	void ExecuteBuildAccelerationStructureCommands(ID3D12CommandQueue* commandQueue);

	void Draw(ID3D12Resource* passCB, ID3D12Resource* matBuffer);

private:
	void BuildOutputTextures();
	void BuildGlobalRootSignature();
	void BuildLocalRootSignature();
	void BuildRayTraceStateObject();

	using ModelBlasList = std::unordered_map<std::string, AccelerationStructureBuffers>;

	ModelBlasList BuildBlases();
	Microsoft::WRL::ComPtr<ID3D12Resource> BuildInstanceBuffer(ModelBlasList& modelBlases);
	AccelerationStructureBuffers BuildTlas(ModelBlasList& modelBlases);
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

	UINT mReflectionMapUavIndex = -1;
	UINT mReflectionMapSrvIndex = -1;

	Microsoft::WRL::ComPtr<ID3D12Resource> mReflectionMap = nullptr;

	std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D12Resource>> mModelBlases;
	Microsoft::WRL::ComPtr<ID3D12Resource> mSceneTlas = nullptr;

	std::unordered_map<std::string, RTModelDef> mModels;
	std::vector<RTInstance> mInstances;

	std::vector<D3D12_RAYTRACING_AABB> mInstanceBounds;

	// shader table
	Microsoft::WRL::ComPtr<ID3D12Resource> mMissShaderTable;
	Microsoft::WRL::ComPtr<ID3D12Resource> mHitGroupShaderTable;
	Microsoft::WRL::ComPtr<ID3D12Resource> mRayGenShaderTable;

	UINT64 mHitGroupShaderTableStrideInBytes = 0;
	UINT64 mMissShaderTableStrideInBytes = 0;
};

 