
#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/GameTimer.h"
#include "../../Common/DescriptorUtil.h"

#include "../../Shaders/SharedTypes.h"


class Terrain
{
public:

	struct InitInfo
	{
		InitInfo()
		{
			ZeroMemory(this, sizeof(InitInfo));
		}

		// Filename of RAW heightmap data.
		std::wstring HeightMapFilename;

		// Scale and offset to apply to heights after they have been
		// loaded from the heightmap.
		float HeightScale;
		float HeightOffset;

		// Dimensions of the heightmap.
		UINT HeightmapWidth;
		UINT HeightmapHeight;

		// The world spacing between heightmap samples. 
		float CellSpacing;

		// The number of material layers.
		UINT NumLayers;
	};

public:

	Terrain(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, const InitInfo& initInfo);
	Terrain(const Terrain& rhs)=delete;
	~Terrain();
	Terrain& operator=(const Terrain& rhs)=delete;

	void BuildDescriptors();

	void SetMaterialLayers(std::initializer_list<Material*> layers,
						   UINT blendMap0SrvIndex, UINT blendMap1SrvIndex);

	void SetMaxTess(float maxTess);
	void SetMinTessDist(float value);
	void SetMaxTessDist(float value);
	
	void SetUseTerrainHeightMap(bool value);
	void SetUseMaterialHeightMaps(bool value);

	float GetWidth()const;
	float GetDepth()const;
	float GetHeight(float x, float z)const;

	DirectX::XMFLOAT4X4 GetWorld()const;
	void SetWorld(const DirectX::XMFLOAT4X4& W);

	void Draw(ID3D12GraphicsCommandList* cmdList, ID3D12PipelineState* drawTerrainPso);
	
private:
	void LoadHeightmapRaw16();
	void CalcAllPatchBoundsY();
	void CalcPatchBoundsY(UINT i, UINT j);
	void BuildQuadPatchVB(DirectX::ResourceUploadBatch& uploadBatch);
	void BuildQuadPatchIB(DirectX::ResourceUploadBatch& uploadBatch);
	void BuildHeightMapTexture(DirectX::ResourceUploadBatch& uploadBatch);

private:

	ID3D12Device* md3dDevice = nullptr;

	// Divide heightmap into patches such that each patch has CellsPerPatch cells
	// and CellsPerPatch+1 vertices.  
	// Note: Can't make this too small without going to 32-bit indices.
	static const int CellsPerPatch = 32;

	Microsoft::WRL::ComPtr<ID3D12Resource> mQuadPatchVB = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mQuadPatchIB = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mHeightMapTexture = nullptr;

	DirectX::GraphicsResource mDrawConstants;

	uint32_t mHeightMapSrvIndex = -1;
	uint32_t mBlendMap0SrvIndex = -1;
	uint32_t mBlendMap1SrvIndex = -1;

	InitInfo mInfo;

	UINT mNumPatchVertices = 0;
	UINT mNumPatchQuadFaces = 0;

	UINT mNumPatchVertRows = 0;
	UINT mNumPatchVertCols = 0;

	DirectX::XMFLOAT4X4 mWorld = MathHelper::Identity4x4();

	std::vector<DirectX::XMFLOAT2> mPatchBoundsY;
	std::vector<float> mHeightmap;

	std::vector<Material*> mLayerMaterials;

	float mMaxTess = 6.0f;
	float mMinTessDist = 20;
	float mMaxTessDist = 250;

	bool mUseTerrainHeightMap = true;
	bool mUseMaterialHeightMaps = true;
};

