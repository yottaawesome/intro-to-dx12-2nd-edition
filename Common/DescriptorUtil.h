#pragma once

#include <windows.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <DirectXColors.h>
#include <DirectXCollision.h>
#include "d3dx12.h"

#include <string>
#include <unordered_set>
#include <memory>
#include <queue>

class DescriptorHeap
{
public:
    DescriptorHeap() = default;
    DescriptorHeap(const DescriptorHeap& rhs) = delete;
    DescriptorHeap& operator=(const DescriptorHeap& rhs) = delete;

    void Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity);

    ID3D12DescriptorHeap* GetD3dHeap()const;

    CD3DX12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t index);
    CD3DX12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t index);

protected:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mHeap = nullptr;
    UINT mDescriptorSize = 0;
};


// For CbvSrvUav. When a resource is created, request a free bindless index. When a resource is destroyed, 
// release the index so that it can be reused by another resource. The main idea is to somewhat automate 
// getting CbvSrvUav descriptors. We do not care where the descriptor is in the heap so long as we have its
// index, we can reference it in the shader.
class CbvSrvUavHeap : public DescriptorHeap
{
public:
	CbvSrvUavHeap(const DescriptorHeap& rhs) = delete;
	CbvSrvUavHeap& operator=(const CbvSrvUavHeap& rhs) = delete;

    static CbvSrvUavHeap& Get()
    {
        static CbvSrvUavHeap singleton;
        return singleton;
    }

    bool IsInitialized()const;

	void Init(ID3D12Device* device, UINT capacity);

	uint32_t NextFreeIndex();
	void ReleaseIndex(uint32_t index);
	
private:
    CbvSrvUavHeap() = default;

private:
    bool mIsInitialized = false;

	std::queue<uint32_t> mFreeIndices;

	// Used for validation. Could put in debug builds only.
	std::unordered_set<uint32_t> mUsedIndices;
};

// Applications usually only need a handful of samplers.  So just define them all
// up front in the sampler heap, and index them in shaders.
class SamplerHeap : public DescriptorHeap
{
public:
    SamplerHeap(const SamplerHeap& rhs) = delete;
    SamplerHeap& operator=(const SamplerHeap& rhs) = delete;

    static SamplerHeap& Get()
    {
        static SamplerHeap singleton;
        return singleton;
    }

    bool IsInitialized()const;

    void Init(ID3D12Device* device);

private:
    SamplerHeap() = default;

    D3D12_SAMPLER_DESC InitSamplerDesc(
        D3D12_FILTER filter = D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE addressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE addressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE addressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        FLOAT mipLODBias = 0,
        UINT maxAnisotropy = 16,
        D3D12_COMPARISON_FUNC comparisonFunc = D3D12_COMPARISON_FUNC_NONE,
        const DirectX::XMFLOAT4& borderColor = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f),
        FLOAT minLOD = 0.f,
        FLOAT maxLOD = D3D12_FLOAT32_MAX);
private:

    bool mIsInitialized = false;
};

inline void CreateDsv(ID3D12Device* device, ID3D12Resource* resource, D3D12_DSV_FLAGS flags, DXGI_FORMAT format, UINT mipSlice, CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor)
{
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
    dsvDesc.Flags = flags;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Format = format;
    dsvDesc.Texture2D.MipSlice = mipSlice;
    device->CreateDepthStencilView(resource, &dsvDesc, hDescriptor);
}

inline void CreateSrv2d(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, UINT mipLevels, CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    srvDesc.Format = format;
    srvDesc.Texture2D.MipLevels = mipLevels;
    device->CreateShaderResourceView(resource, &srvDesc, hDescriptor);
}

inline void CreateSrv2dArray(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, UINT mipLevels, UINT arraySize, CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = arraySize;
    srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    srvDesc.Format = format;
    srvDesc.Texture2DArray.MipLevels = mipLevels;
    device->CreateShaderResourceView(resource, &srvDesc, hDescriptor);
}

inline void CreateSrvCube(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, UINT mipLevels, CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
    srvDesc.Format = format;
    srvDesc.TextureCube.MipLevels = mipLevels;
    device->CreateShaderResourceView(resource, &srvDesc, hDescriptor);
}

inline void CreateRtv2d(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, UINT mipSlice, CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor)
{
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Format = format;
    rtvDesc.Texture2D.MipSlice = mipSlice;
    rtvDesc.Texture2D.PlaneSlice = 0;
    device->CreateRenderTargetView(resource, &rtvDesc, hDescriptor);
}

inline void CreateUav2d(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, UINT mipSlice, CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = format;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = mipSlice;
    device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, hDescriptor);
}

inline void CreateBufferUav(ID3D12Device* device, UINT64 firstElement, UINT elementCount, 
                            UINT elementByteSize, UINT64 counterOffset,
                            ID3D12Resource* resource, ID3D12Resource* counterResource, 
                            CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN; // structured buffer
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = firstElement;
    uavDesc.Buffer.NumElements = elementCount;
    uavDesc.Buffer.StructureByteStride = elementByteSize;
    uavDesc.Buffer.CounterOffsetInBytes = counterOffset;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(resource, counterResource, &uavDesc, hDescriptor);
}

inline void CreateBufferSrv(ID3D12Device* device, UINT64 firstElement, UINT elementCount, UINT elementByteSize,
                            ID3D12Resource* resource, CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN; // structured buffer
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = firstElement;
    srvDesc.Buffer.NumElements = elementCount;
    srvDesc.Buffer.StructureByteStride = elementByteSize;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(resource, &srvDesc, hDescriptor);
}
