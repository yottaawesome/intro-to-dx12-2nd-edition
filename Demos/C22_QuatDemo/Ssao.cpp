//***************************************************************************************
// Ssao.cpp by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

#include "Ssao.h"
#include "../../Common/DescriptorUtil.h"

using namespace DirectX;
using namespace Microsoft::WRL;

Ssao::Ssao(ID3D12Device* device, UINT width, UINT height)
{
    md3dDevice = device;

	BuildOffsetVectors();

    mBlurWeights = d3dUtil::CalcGaussWeights(2.5f);

    UpdateSize(width, height);

    BuildResources();
}

float Ssao::GetOcclusionRadius()const
{
    return mOcclusionRadius;
}

float Ssao::GetOcclusionFadeStart()const
{
    return mOcclusionFadeStart;
}

float Ssao::GetOcclusionFadeEnd()const
{
    return mOcclusionFadeEnd;
}

float Ssao::GetSurfaceEpsilon()const
{
    return mSurfaceEpsilon;
}

void Ssao::SetOcclusionRadius(float value)
{
    if(mOcclusionRadius != value)
    {
        mOcclusionRadius = value;
        mSsaoConstantsDirty = true;
    }
}

void Ssao::SetOcclusionFadeStart(float value)
{
    if(mOcclusionFadeStart != value)
    {
        mOcclusionFadeStart = value;
        mSsaoConstantsDirty = true;
    }
}

void Ssao::SetOcclusionFadeEnd(float value)
{
    if(mOcclusionFadeEnd != value)
    {
        mOcclusionFadeEnd = value;
        mSsaoConstantsDirty = true;
    }
}

void Ssao::SetSurfaceEpsilon(float value)
{
    if(mSurfaceEpsilon != value)
    {
        mSurfaceEpsilon = value;
        mSsaoConstantsDirty = true;
    }
}

UINT Ssao::SsaoMapWidth()const
{
    return mRenderTargetWidth / 2;
}

UINT Ssao::SsaoMapHeight()const
{
    return mRenderTargetHeight / 2;
}

ID3D12Resource* Ssao::NormalMap()
{
    return mNormalMap.Get();
}

CD3DX12_CPU_DESCRIPTOR_HANDLE Ssao::NormalMapRtv()const
{
    return mhNormalMapCpuRtv;
}

uint32_t Ssao::GetNormalMapBindlessIndex()const
{
    return mNormalMapBindlessIndex;
}

uint32_t Ssao::GetAmbientMap0BindlessIndex()const
{
    return mAmbientMap0BindlessIndex;
}

uint32_t Ssao::GetAmbientMap1BindlessIndex()const
{
    return mAmbientMap1BindlessIndex;
}

void Ssao::BuildDescriptors(
    CD3DX12_CPU_DESCRIPTOR_HANDLE hNormalMapCpuRtv,
    CD3DX12_CPU_DESCRIPTOR_HANDLE hAmbientMap0CpuRtv,
    CD3DX12_CPU_DESCRIPTOR_HANDLE hAmbientMap1CpuRtv)
{
    // RTVs are in a separate RTV heap.
    mhNormalMapCpuRtv = hNormalMapCpuRtv;
    mhAmbientMap0CpuRtv = hAmbientMap0CpuRtv;
    mhAmbientMap1CpuRtv = hAmbientMap1CpuRtv;

    CbvSrvUavHeap& bindlessHeap = CbvSrvUavHeap::Get();
    mNormalMapBindlessIndex = bindlessHeap.NextFreeIndex();
    mhNormalMapCpuSrv = bindlessHeap.CpuHandle(mNormalMapBindlessIndex);
    mhNormalMapGpuSrv = bindlessHeap.GpuHandle(mNormalMapBindlessIndex);

    mAmbientMap0BindlessIndex = bindlessHeap.NextFreeIndex();
    mhAmbientMap0CpuSrv = bindlessHeap.CpuHandle(mAmbientMap0BindlessIndex);
    mhAmbientMap0GpuSrv = bindlessHeap.GpuHandle(mAmbientMap0BindlessIndex);

    mAmbientMap1BindlessIndex = bindlessHeap.NextFreeIndex();
    mhAmbientMap1CpuSrv = bindlessHeap.CpuHandle(mAmbientMap1BindlessIndex);
    mhAmbientMap1GpuSrv = bindlessHeap.GpuHandle(mAmbientMap1BindlessIndex);

    // Create the descriptors
    BuildDescriptors();
}

void Ssao::OnResize(UINT newWidth, UINT newHeight)
{
    if(mRenderTargetWidth != newWidth || mRenderTargetHeight != newHeight)
    {
        UpdateSize(newWidth, newHeight);

        BuildResources();
        BuildDescriptors();
    }
}

void Ssao::ComputeSsao(ID3D12GraphicsCommandList* cmdList, ID3D12PipelineState* ssaoPso)
{
	cmdList->RSSetViewports(1, &mViewport);
    cmdList->RSSetScissorRects(1, &mScissorRect);

    if(mSsaoConstantsDirty)
    {
        UpdateConstants();

        // Allocate new memory.
        GraphicsMemory& linearAllocator = GraphicsMemory::Get(md3dDevice);
        mMemHandleSsaoHorzCB = linearAllocator.AllocateConstant(mSsaoHorzConstants);
        mMemHandleSsaoVertCB = linearAllocator.AllocateConstant(mSsaoVertConstants);
    }

    // For SSAO pass, we can use either cbuffer since this shader does not use gHorzBlur. 
    cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, mMemHandleSsaoHorzCB.GpuAddress());

	// We compute the initial SSAO to AmbientMap0.

    // Change to RENDER_TARGET.
    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mAmbientMap0.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
  
	float clearValue[] = {1.0f, 1.0f, 1.0f, 1.0f};
    cmdList->ClearRenderTargetView(mhAmbientMap0CpuRtv, clearValue, 0, nullptr);
     
	// Specify the buffers we are going to render to.
    cmdList->OMSetRenderTargets(1, &mhAmbientMap0CpuRtv, true, nullptr);

    cmdList->SetPipelineState(ssaoPso);

	// Draw fullscreen quad.
	cmdList->IASetVertexBuffers(0, 0, nullptr);
    cmdList->IASetIndexBuffer(nullptr);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->DrawInstanced(6, 1, 0, 0);
   
	// Change back to GENERIC_READ so we can read the texture in a shader.
    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mAmbientMap0.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
}
 
void Ssao::BlurAmbientMap(ID3D12GraphicsCommandList* cmdList, ID3D12PipelineState* ssaoBlurPso, int blurCount)
{
    cmdList->SetPipelineState(ssaoBlurPso);

    for(int i = 0; i < blurCount; ++i)
    {
        BlurAmbientMap(cmdList, true);
        BlurAmbientMap(cmdList, false);
    }
}

void Ssao::BlurAmbientMap(ID3D12GraphicsCommandList* cmdList, bool horzBlur)
{
	ID3D12Resource* output = nullptr;
	CD3DX12_CPU_DESCRIPTOR_HANDLE outputRtv;
	
	// Ping-pong the two ambient map textures as we apply
	// horizontal and vertical blur passes.
	if(horzBlur == true)
	{
		output = mAmbientMap1.Get();
		outputRtv = mhAmbientMap1CpuRtv;
        cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, mMemHandleSsaoHorzCB.GpuAddress());
	}
	else
	{
		output = mAmbientMap0.Get();
		outputRtv = mhAmbientMap0CpuRtv;
        cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, mMemHandleSsaoVertCB.GpuAddress());
	}

    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(output,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

	float clearValue[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    cmdList->ClearRenderTargetView(outputRtv, clearValue, 0, nullptr);
 
    cmdList->OMSetRenderTargets(1, &outputRtv, true, nullptr);
	
	// Draw fullscreen quad.
	cmdList->IASetVertexBuffers(0, 0, nullptr);
    cmdList->IASetIndexBuffer(nullptr);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->DrawInstanced(6, 1, 0, 0);
   
    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(output,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
}
 
void Ssao::UpdateSize(UINT width, UINT height)
{
    mRenderTargetWidth = width;
    mRenderTargetHeight = height;

    // We render to ambient map at half the resolution.
    mViewport.TopLeftX = 0.0f;
    mViewport.TopLeftY = 0.0f;
    mViewport.Width = mRenderTargetWidth / 2.0f;
    mViewport.Height = mRenderTargetHeight / 2.0f;
    mViewport.MinDepth = 0.0f;
    mViewport.MaxDepth = 1.0f;

    mScissorRect = { 0, 0, (int)mRenderTargetWidth / 2, (int)mRenderTargetHeight / 2 };
}

void Ssao::UpdateConstants()
{
    std::copy(&mOffsets[0], &mOffsets[14], &mSsaoHorzConstants.gOffsetVectors[0]);

    mSsaoHorzConstants.gBlurWeights[0] = XMFLOAT4(&mBlurWeights[0]);
    mSsaoHorzConstants.gBlurWeights[1] = XMFLOAT4(&mBlurWeights[4]);
    mSsaoHorzConstants.gBlurWeights[2] = XMFLOAT4(&mBlurWeights[8]);

    // Coordinates given in view space.
    mSsaoHorzConstants.gOcclusionRadius = mOcclusionRadius;
    mSsaoHorzConstants.gOcclusionFadeStart = mOcclusionFadeStart;
    mSsaoHorzConstants.gOcclusionFadeEnd = mOcclusionFadeEnd;
    mSsaoHorzConstants.gSurfaceEpsilon = mSurfaceEpsilon;

    const float ambientMapWidth = static_cast<float>(mAmbientMap0->GetDesc().Width);
    const float ambientMapHeight = static_cast<float>(mAmbientMap0->GetDesc().Height);
    mSsaoHorzConstants.gInvAmbientMapSize = XMFLOAT2(1.0f / ambientMapWidth, 1.0f / ambientMapHeight);

    mSsaoHorzConstants.gHorzBlur = 1;

    mSsaoVertConstants = mSsaoHorzConstants;
    mSsaoVertConstants.gHorzBlur = 0;
}

void Ssao::BuildDescriptors()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Format = SceneNormalMapFormat;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    md3dDevice->CreateShaderResourceView(mNormalMap.Get(), &srvDesc, mhNormalMapCpuSrv);

    srvDesc.Format = SsaoAmbientMapFormat;
    md3dDevice->CreateShaderResourceView(mAmbientMap0.Get(), &srvDesc, mhAmbientMap0CpuSrv);
    md3dDevice->CreateShaderResourceView(mAmbientMap1.Get(), &srvDesc, mhAmbientMap1CpuSrv);

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Format = SceneNormalMapFormat;
    rtvDesc.Texture2D.MipSlice = 0;
    rtvDesc.Texture2D.PlaneSlice = 0;
    md3dDevice->CreateRenderTargetView(mNormalMap.Get(), &rtvDesc, mhNormalMapCpuRtv);

    rtvDesc.Format = SsaoAmbientMapFormat;
    md3dDevice->CreateRenderTargetView(mAmbientMap0.Get(), &rtvDesc, mhAmbientMap0CpuRtv);
    md3dDevice->CreateRenderTargetView(mAmbientMap1.Get(), &rtvDesc, mhAmbientMap1CpuRtv);
}

void Ssao::BuildResources()
{
	// Free the old resources if they exist.
    mNormalMap = nullptr;
    mAmbientMap0 = nullptr;
    mAmbientMap1 = nullptr;

    D3D12_RESOURCE_DESC texDesc;
    ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = mRenderTargetWidth;
    texDesc.Height = mRenderTargetHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = SceneNormalMapFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;


    float normalClearColor[] = { 0.0f, 0.0f, 1.0f, 0.0f };
    CD3DX12_CLEAR_VALUE optClear(SceneNormalMapFormat, normalClearColor);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &optClear,
        IID_PPV_ARGS(&mNormalMap)));

	// Ambient occlusion maps are at half resolution.
    texDesc.Width = mRenderTargetWidth / 2;
    texDesc.Height = mRenderTargetHeight / 2;
    texDesc.Format = SsaoAmbientMapFormat;

    float ambientClearColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    optClear = CD3DX12_CLEAR_VALUE(SsaoAmbientMapFormat, ambientClearColor);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &optClear,
        IID_PPV_ARGS(&mAmbientMap0)));

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        &optClear,
        IID_PPV_ARGS(&mAmbientMap1)));
}

void Ssao::BuildOffsetVectors()
{
    // Start with 14 uniformly distributed vectors.  We choose the 8 corners of the cube
	// and the 6 center points along each cube face.  We always alternate the points on 
	// opposites sides of the cubes.  This way we still get the vectors spread out even
	// if we choose to use less than 14 samples.
	
	// 8 cube corners
	mOffsets[0] = XMFLOAT4(+1.0f, +1.0f, +1.0f, 0.0f);
	mOffsets[1] = XMFLOAT4(-1.0f, -1.0f, -1.0f, 0.0f);

	mOffsets[2] = XMFLOAT4(-1.0f, +1.0f, +1.0f, 0.0f);
	mOffsets[3] = XMFLOAT4(+1.0f, -1.0f, -1.0f, 0.0f);

	mOffsets[4] = XMFLOAT4(+1.0f, +1.0f, -1.0f, 0.0f);
	mOffsets[5] = XMFLOAT4(-1.0f, -1.0f, +1.0f, 0.0f);

	mOffsets[6] = XMFLOAT4(-1.0f, +1.0f, -1.0f, 0.0f);
	mOffsets[7] = XMFLOAT4(+1.0f, -1.0f, +1.0f, 0.0f);

	// 6 centers of cube faces
	mOffsets[8] = XMFLOAT4(-1.0f, 0.0f, 0.0f, 0.0f);
	mOffsets[9] = XMFLOAT4(+1.0f, 0.0f, 0.0f, 0.0f);

	mOffsets[10] = XMFLOAT4(0.0f, -1.0f, 0.0f, 0.0f);
	mOffsets[11] = XMFLOAT4(0.0f, +1.0f, 0.0f, 0.0f);

	mOffsets[12] = XMFLOAT4(0.0f, 0.0f, -1.0f, 0.0f);
	mOffsets[13] = XMFLOAT4(0.0f, 0.0f, +1.0f, 0.0f);

    for(int i = 0; i < 14; ++i)
	{
		// Create random lengths in [0.25, 1.0].
		float s = MathHelper::RandF(0.25f, 1.0f);
		
		XMVECTOR v = s * XMVector4Normalize(XMLoadFloat4(&mOffsets[i]));
		
		XMStoreFloat4(&mOffsets[i], v);
	}
}

