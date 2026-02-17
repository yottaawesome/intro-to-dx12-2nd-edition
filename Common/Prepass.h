//***************************************************************************************
// Prepass.h by Frank Luna (C) 2023 All Rights Reserved.
//***************************************************************************************

#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Common/UploadBuffer.h"

// Manages 
class Prepass
{
public:

	Prepass(ID3D12Device* device);
		
	Prepass(const Prepass& rhs)=delete;
	Prepass& operator=(const Prepass& rhs)=delete;
	~Prepass()=default;

	ID3D12Resource* GetSceneNormalMap()const;
	UINT GetSceneNormalMapBindlessIndex()const;
	UINT GetSceneDepthMapBindlessIndex()const;
	CD3DX12_CPU_DESCRIPTOR_HANDLE GetSceneNormalMapRtv()const;

	void AllocateDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE sceneNormalMapRtv);
	void OnResize(UINT newWidth, UINT newHeight, ID3D12Resource* depthStencilBuffer);

private:
	void BuildResources();
	void BuildDescriptors(ID3D12Resource* depthStencilBuffer);

private:

	ID3D12Device* md3dDevice = nullptr;

	UINT mWidth = 0;
	UINT mHeight = 0;

	CD3DX12_CPU_DESCRIPTOR_HANDLE mSceneNormalMapRtv;

	Microsoft::WRL::ComPtr<ID3D12Resource> mSceneNormalMap = nullptr;
	uint32_t mSceneNormalMapBindlessIndex = -1;
	uint32_t mSceneDepthMapBindlessIndex = -1;
};

 