
#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Shaders/SharedTypes.h"

class BlurFilter
{
public:
	///<summary>
	/// The width and height should match the dimensions of the input texture to blur.
	/// Recreate when the screen is resized. 
	///</summary>
	BlurFilter(ID3D12Device* device, 
		UINT width, UINT height,
		DXGI_FORMAT format);
		
	BlurFilter(const BlurFilter& rhs)=delete;
	BlurFilter& operator=(const BlurFilter& rhs)=delete;
	~BlurFilter()=default;

	ID3D12Resource* Output();

	void BuildDescriptors();

	void OnResize(UINT newWidth, UINT newHeight);

	///<summary>
	/// Blurs the input texture blurCount times.
	///</summary>
	void Execute(
		ID3D12GraphicsCommandList* cmdList, 
		ID3D12RootSignature* rootSig,
		ID3D12Resource* passCB,
		ID3D12PipelineState* horzBlurPSO,
		ID3D12PipelineState* vertBlurPSO,
		ID3D12Resource* input, 		
		int blurCount,
		float blurSigma);

private:
	std::vector<float> CalcGaussWeights(float sigma);

	void BuildResources();

private:

	const int MaxBlurRadius = 15;

	ID3D12Device* md3dDevice = nullptr;

	UINT mWidth = 0;
	UINT mHeight = 0;
	DXGI_FORMAT mFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	DirectX::GraphicsResource mHorzPassConstants;
	DirectX::GraphicsResource mVertPassConstants;

	uint32_t mBlur0SrvIndex = -1;
	uint32_t mBlur1SrvIndex = -1;

	uint32_t mBlur0UavIndex = -1;
	uint32_t mBlur1UavIndex = -1;

	// Two for ping-ponging the textures.
	Microsoft::WRL::ComPtr<ID3D12Resource> mBlurMap0 = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mBlurMap1 = nullptr;
};
