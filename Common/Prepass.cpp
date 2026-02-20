//***************************************************************************************
// Prepass.cpp by Frank Luna (C) 2023 All Rights Reserved.
//***************************************************************************************

#include "Prepass.h"
#include "DescriptorUtil.h"
#include "d3dApp.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;
using namespace DirectX::SimpleMath;

Prepass::Prepass(ID3D12Device* device) :
    md3dDevice(device)
{
}

ID3D12Resource* Prepass::GetSceneNormalMap()const
{
    return mSceneNormalMap.Get();
}

UINT Prepass::GetSceneNormalMapBindlessIndex()const
{
    return mSceneNormalMapBindlessIndex;
}

UINT Prepass::GetSceneDepthMapBindlessIndex()const
{
    return mSceneDepthMapBindlessIndex;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE Prepass::GetSceneNormalMapRtv()const
{
    return mSceneNormalMapRtv;
}

void Prepass::AllocateDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE sceneNormalMapRtv)
{
    mSceneNormalMapRtv = sceneNormalMapRtv;

    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    assert(cbvSrvUavHeap.IsInitialized());

    mSceneNormalMapBindlessIndex = cbvSrvUavHeap.NextFreeIndex();

    // Create SRV to depth buffer so we can sample it in a shader. When we start to sample from it,
    // we need to be done writing to it.
    mSceneDepthMapBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
}

void Prepass::OnResize(UINT newWidth, UINT newHeight, ID3D12Resource* depthStencilBuffer)
{
    mWidth = newWidth;
    mHeight = newHeight;

    BuildResources();
    BuildDescriptors(depthStencilBuffer);
}

void Prepass::BuildResources()
{
    // Free the old resources if they exist.
    mSceneNormalMap = nullptr;

    D3D12_RESOURCE_DESC texDesc;
    ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = mWidth;
    texDesc.Height = mHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = SceneNormalMapFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    float normalClearColor[] = { 0.0f, 0.0f, 1.0f, 0.0f };
    CD3DX12_CLEAR_VALUE optClear(SceneNormalMapFormat, normalClearColor);
	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &optClear,
        IID_PPV_ARGS(&mSceneNormalMap)));
}

void Prepass::BuildDescriptors(ID3D12Resource* depthStencilBuffer)
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    assert(cbvSrvUavHeap.IsInitialized());

    CreateSrv2d(md3dDevice, mSceneNormalMap.Get(), SceneNormalMapFormat, 1, cbvSrvUavHeap.CpuHandle(mSceneNormalMapBindlessIndex));
    CreateRtv2d(md3dDevice, mSceneNormalMap.Get(), SceneNormalMapFormat, 0, mSceneNormalMapRtv);
    CreateSrv2d(md3dDevice, depthStencilBuffer, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1, cbvSrvUavHeap.CpuHandle(mSceneDepthMapBindlessIndex));
}