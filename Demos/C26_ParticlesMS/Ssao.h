//***************************************************************************************
// Ssao.h by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#ifndef SSAO_H
#define SSAO_H

#pragma once

#include "../../Common/d3dUtil.h"
#include "FrameResource.h"
 
 
class Ssao
{
public:
	Ssao(ID3D12Device* device, UINT width, UINT height);

    Ssao(const Ssao& rhs) = delete;
    Ssao& operator=(const Ssao& rhs) = delete;
    ~Ssao() = default; 

    static const int MaxBlurRadius = 5;

    float GetOcclusionRadius()const;
    float GetOcclusionFadeStart()const;
    float GetOcclusionFadeEnd()const;
    float GetSurfaceEpsilon()const;

    void SetOcclusionRadius(float value);
    void SetOcclusionFadeStart(float value);
    void SetOcclusionFadeEnd(float value);
    void SetSurfaceEpsilon(float value);

	UINT SsaoMapWidth()const;
    UINT SsaoMapHeight()const;

    ID3D12Resource* NormalMap();
    CD3DX12_CPU_DESCRIPTOR_HANDLE NormalMapRtv()const;

    uint32_t GetNormalMapBindlessIndex()const;
    uint32_t GetAmbientMap0BindlessIndex()const;
    uint32_t GetAmbientMap1BindlessIndex()const;

    void BuildDescriptors(
        CD3DX12_CPU_DESCRIPTOR_HANDLE hNormalMapCpuRtv,
        CD3DX12_CPU_DESCRIPTOR_HANDLE hAmbientMap0CpuRtv,
        CD3DX12_CPU_DESCRIPTOR_HANDLE hAmbientMap1CpuRtv);

	void OnResize(UINT newWidth, UINT newHeight);
  
    ///<summary>
    /// Changes the render target to the Ambient render target and draws a fullscreen
    /// quad to kick off the pixel shader to compute the AmbientMap.  We still keep the
    /// main depth buffer bound to the pipeline, but depth buffer read/writes
    /// are disabled, as we do not need the depth buffer when computing the Ambient map.
    ///</summary>
	void ComputeSsao(ID3D12GraphicsCommandList* cmdList, ID3D12PipelineState* ssaoPso);

    ///<summary>
    /// Blurs the ambient map to smooth out the noise caused by only taking a
    /// few random samples per pixel.  We use an edge preserving blur so that 
    /// we do not blur across discontinuities--we want edges to remain edges.
    ///</summary>
    void BlurAmbientMap(ID3D12GraphicsCommandList* cmdList, ID3D12PipelineState* ssaoBlurPso, int blurCount);
private:
    
	void BlurAmbientMap(ID3D12GraphicsCommandList* cmdList, bool horzBlur);

    void UpdateSize(UINT width, UINT height);
    void UpdateConstants();
    void BuildDescriptors();
    void BuildResources();
    void BuildOffsetVectors();

private:
	ID3D12Device* md3dDevice = nullptr;

    // Two sets of constants for ping-pong blurring the ambient map. They are the same
    // except for the gHorzBlur constant. The main SSAO pass can use either since the shader
    // does not use gHorzBlur. 
    bool mSsaoConstantsDirty = true;
    SsaoCB mSsaoHorzConstants;
    SsaoCB mSsaoVertConstants;
    DirectX::GraphicsResource mMemHandleSsaoHorzCB;
    DirectX::GraphicsResource mMemHandleSsaoVertCB;

    std::vector<float> mBlurWeights;
    float mOcclusionRadius = 0.5f;
    float mOcclusionFadeStart = 0.2f;
    float mOcclusionFadeEnd = 1.0f;
    float mSurfaceEpsilon =  0.05f;

    D3D12_VIEWPORT mViewport;
    D3D12_RECT mScissorRect;

    Microsoft::WRL::ComPtr<ID3D12Resource> mNormalMap = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAmbientMap0 = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAmbientMap1 = nullptr;

    uint32_t mNormalMapBindlessIndex = -1;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhNormalMapCpuSrv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhNormalMapGpuSrv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhNormalMapCpuRtv;

    // Need two for ping-ponging during blur.
    uint32_t mAmbientMap0BindlessIndex = -1;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhAmbientMap0CpuSrv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhAmbientMap0GpuSrv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhAmbientMap0CpuRtv;

    uint32_t mAmbientMap1BindlessIndex = -1;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhAmbientMap1CpuSrv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhAmbientMap1GpuSrv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhAmbientMap1CpuRtv;

	UINT mRenderTargetWidth= 0;
	UINT mRenderTargetHeight= 0;

    DirectX::XMFLOAT4 mOffsets[14];
};

#endif // SSAO_H