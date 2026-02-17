//***************************************************************************************
// ShadowMap.cpp by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

#include "ShadowMap.h"
#include "../../Common/DescriptorUtil.h"

ShadowMap::ShadowMap(ID3D12Device* device, UINT width, UINT height)
{
    md3dDevice = device;

    mWidth = width;
    mHeight = height;

    mViewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    mScissorRect = { 0, 0, (int)width, (int)height };

    BuildResource();
}

UINT ShadowMap::Width()const
{
    return mWidth;
}

UINT ShadowMap::Height()const
{
    return mHeight;
}

ID3D12Resource* ShadowMap::Resource()
{
    return mShadowMap.Get();
}

uint32_t ShadowMap::BindlessIndex()const
{
    return mBindlessIndex;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE ShadowMap::Srv()const
{
    return mhGpuSrv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE ShadowMap::Dsv()const
{
    return mhCpuDsv;
}

D3D12_VIEWPORT ShadowMap::Viewport()const
{
    return mViewport;
}

D3D12_RECT ShadowMap::ScissorRect()const
{
    return mScissorRect;
}

uint32_t ShadowMap::BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuDsv)
{
    CbvSrvUavHeap& bindlessHeap = CbvSrvUavHeap::Get();

    mBindlessIndex = bindlessHeap.NextFreeIndex();

    // Save references to the descriptors.
    mhCpuSrv = bindlessHeap.CpuHandle(mBindlessIndex);
    mhGpuSrv = bindlessHeap.GpuHandle(mBindlessIndex);
    mhCpuDsv = hCpuDsv;

    //  Create the descriptors
    BuildDescriptors();

    return mBindlessIndex;
}

void ShadowMap::OnResize(UINT newWidth, UINT newHeight)
{
    if((mWidth != newWidth) || (mHeight != newHeight))
    {
        mWidth = newWidth;
        mHeight = newHeight;

        BuildResource();

        // New resource, so we need new descriptors to that resource.
        BuildDescriptors();
    }
}

void ShadowMap::BuildDescriptors()
{
    // Create SRV to resource so we can sample the shadow map in a shader program.
    CreateSrv2d(md3dDevice, mShadowMap.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1, mhCpuSrv);

    // Create DSV to resource so we can render to the shadow map.
    CreateDsv(md3dDevice, mShadowMap.Get(), D3D12_DSV_FLAG_NONE, DXGI_FORMAT_D24_UNORM_S8_UINT, 0, mhCpuDsv);
}

void ShadowMap::BuildResource()
{
    D3D12_RESOURCE_DESC texDesc;
    ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = mWidth;
    texDesc.Height = mHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = mFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear;
    optClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &optClear,
        IID_PPV_ARGS(&mShadowMap)));
}