
#include "DescriptorUtil.h"
#include "d3dUtil.h"
#include <algorithm>

using namespace DirectX;

void DescriptorHeap::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity)
{
    assert(mHeap == nullptr);

   D3D12_DESCRIPTOR_HEAP_DESC heapDesc;
   heapDesc.NumDescriptors = capacity;
   heapDesc.Type = type;
   heapDesc.Flags = type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ?
       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : 
       D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
   heapDesc.NodeMask = 0;
   ThrowIfFailed(device->CreateDescriptorHeap(
       &heapDesc, IID_PPV_ARGS(mHeap.GetAddressOf())));

    mDescriptorSize = device->GetDescriptorHandleIncrementSize(type);
}

ID3D12DescriptorHeap* DescriptorHeap::GetD3dHeap()const
{
    return mHeap.Get();
}

CD3DX12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::CpuHandle(uint32_t index)
{
    auto hcpu = CD3DX12_CPU_DESCRIPTOR_HANDLE(mHeap->GetCPUDescriptorHandleForHeapStart());
    hcpu.Offset(index, mDescriptorSize);
    return hcpu;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GpuHandle(uint32_t index)
{
    auto hgpu = CD3DX12_GPU_DESCRIPTOR_HANDLE(mHeap->GetGPUDescriptorHandleForHeapStart());
    hgpu.Offset(index, mDescriptorSize);
    return hgpu;
}

bool CbvSrvUavHeap::IsInitialized()const
{
    return mIsInitialized;
}

void CbvSrvUavHeap::Init(ID3D12Device* device, UINT capacity)
{
    DescriptorHeap::Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, capacity);

    for(UINT i = 0; i < capacity; ++i)
        mFreeIndices.push(i);

    mUsedIndices.clear();
    mIsInitialized = true;
}

uint32_t CbvSrvUavHeap::NextFreeIndex()
{
    assert(!mFreeIndices.empty());

    const uint32_t index = mFreeIndices.front();

    mUsedIndices.insert(index);

    mFreeIndices.pop();

    return index;
}

void CbvSrvUavHeap::ReleaseIndex(uint32_t index)
{
    // If a resource is destroyed, we can reuse its index.

    auto it = mUsedIndices.find(index);

    // Make sure we are releasing a used index.
    assert(it != std::end(mUsedIndices));

    mUsedIndices.erase(it);

    mFreeIndices.push(index);
}

bool SamplerHeap::IsInitialized()const
{
    return mIsInitialized;
}

D3D12_SAMPLER_DESC SamplerHeap::InitSamplerDesc(
    D3D12_FILTER filter,
    D3D12_TEXTURE_ADDRESS_MODE addressU,
    D3D12_TEXTURE_ADDRESS_MODE addressV,
    D3D12_TEXTURE_ADDRESS_MODE addressW,
    FLOAT mipLODBias,
    UINT maxAnisotropy,
    D3D12_COMPARISON_FUNC comparisonFunc,
    const XMFLOAT4& borderColor,
    FLOAT minLOD,
    FLOAT maxLOD)
{
    D3D12_SAMPLER_DESC desc;

    desc.Filter = filter;
    desc.AddressU = addressU;
    desc.AddressV = addressV;
    desc.AddressW = addressW;
    desc.MipLODBias = mipLODBias;
    desc.MaxAnisotropy = maxAnisotropy;
    desc.ComparisonFunc = comparisonFunc;
    desc.BorderColor[0] = borderColor.x;
    desc.BorderColor[1] = borderColor.y;
    desc.BorderColor[2] = borderColor.z;
    desc.BorderColor[3] = borderColor.w;
    desc.MinLOD = minLOD;
    desc.MaxLOD = maxLOD;

    return desc;
}

void SamplerHeap::Init(ID3D12Device* device)
{
    if(mIsInitialized)
        return;

    // bump as needed
    const uint32_t capacity = 16;

    DescriptorHeap::Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, capacity);

    const D3D12_SAMPLER_DESC pointWrap = InitSamplerDesc(
        D3D12_FILTER_MIN_MAG_MIP_POINT,    // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,   // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,   // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);  // addressW

    const D3D12_SAMPLER_DESC pointClamp = InitSamplerDesc(
        D3D12_FILTER_MIN_MAG_MIP_POINT,    // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const D3D12_SAMPLER_DESC linearWrap = InitSamplerDesc(
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,  // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const D3D12_SAMPLER_DESC linearClamp = InitSamplerDesc(
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,   // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const D3D12_SAMPLER_DESC anisotropicWrap = InitSamplerDesc(
        D3D12_FILTER_ANISOTROPIC,         // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
        0.0f,                             // mipLODBias
        8);                               // maxAnisotropy

    const D3D12_SAMPLER_DESC anisotropicClamp = InitSamplerDesc(
        D3D12_FILTER_ANISOTROPIC,          // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
        0.0f,                              // mipLODBias
        8);                                // maxAnisotropy

    const D3D12_SAMPLER_DESC shadow = InitSamplerDesc(
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,                // addressU
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,                // addressV
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,                // addressW
        0.0f,                                             // mipLODBias
        16,                                               // maxAnisotropy
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f));

    D3D12_SAMPLER_DESC samplers[] =
    {
        pointWrap,
        pointClamp,
        linearWrap,
        linearClamp,
        anisotropicWrap,
        anisotropicClamp,
        shadow,
    };

    for(int i = 0; i < _countof(samplers); ++i)
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE h = CpuHandle(i);
        device->CreateSampler(&samplers[i], h);
    }
}



