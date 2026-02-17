//***************************************************************************************
// ParticleSystem.h by Frank Luna (C) 2022 All Rights Reserved.
//***************************************************************************************

#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/GameTimer.h"
#include "../../Common/DescriptorUtil.h"

#include "../../Shaders/SharedTypes.h"


class TerrainMS
{
public:
	using Vector2 = DirectX::SimpleMath::Vector2;
	using Matrix = DirectX::SimpleMath::Matrix;

	struct InitInfo
	{
		InitInfo()
		{
			ZeroMemory(this, sizeof(InitInfo));
		}

		std::wstring HeightMapFilename;
		float HeightScale;
		float HeightOffset;
		UINT HeightmapWidth;
		UINT HeightmapHeight;
		float CellSpacing;
		UINT NumLayers;
	};

public:

	TerrainMS(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, const InitInfo& initInfo);
	TerrainMS(const TerrainMS& rhs)=delete;
	~TerrainMS();
	TerrainMS& operator=(const TerrainMS& rhs)=delete;

	void BuildDescriptors();

	void SetMaterialLayers(std::initializer_list<Material*> layers,
						   UINT blendMap0SrvIndex, UINT blendMap1SrvIndex);

	void SetSkirtOffsetY(float value);
	void SetMinTessDist(float value);
	void SetMaxTessDist(float value);
	void SetMaxTess(float maxTess);

	float GetWidth()const;
	float GetDepth()const;
	float GetHeight(float x, float z)const;

	Matrix GetWorld()const;
	void SetWorld(const Matrix& W);

	void Draw(ID3D12GraphicsCommandList6* cmdList, 
			  ID3D12PipelineState* drawTerrainPso,
			  ID3D12PipelineState* drawTerrainSkirtPso,
			  bool drawSkirts);
	
private:
	void LoadHeightmapRaw16();
	void CalcAllQuadGroupBounds();
	DirectX::BoundingBox CalcQuadGroupBounds(UINT groupX, UINT groupY);
	void CalcAllQuadPatchBoundsY();
	void CalcQuadPatchBoundsY(UINT i, UINT j);
	void BuildQuadPatchVB(DirectX::ResourceUploadBatch& uploadBatch);
	void BuildQuadGroupBoundsBuffer(DirectX::ResourceUploadBatch& uploadBatch);
	void BuildHeightMapTexture(DirectX::ResourceUploadBatch& uploadBatch);

private:

	ID3D12Device* md3dDevice = nullptr;

	// Divide heightmap into quad patches such that each quad patch has 
	// CellsPerQuadPatch cells and CellsPerQuadPatch+1 vertices.  
	static const int CellsPerQuadPatch = 32;

	Microsoft::WRL::ComPtr<ID3D12Resource> mQuadGroupBoundsBuffer = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mQuadPatchVB = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> mHeightMapTexture = nullptr;

	DirectX::GraphicsResource mDrawConstants;

	uint32_t mHeightMapSrvIndex = -1;
	uint32_t mBlendMap0SrvIndex = -1;
	uint32_t mBlendMap1SrvIndex = -1;

	uint32_t mTerrainVerticesSrvIndex = -1;
	uint32_t mTerrainGroupBoundsSrvIndex = -1;

	InitInfo mInfo;

	UINT mNumPatchVertices = 0;
	UINT mNumPatchQuadFaces = 0;

	UINT mNumPatchVertRows = 0;
	UINT mNumPatchVertCols = 0;

	// Each thread in the group processes a quad patch.
	const UINT mNumQuadsPerGroupX = 8;
	const UINT mNumQuadsPerGroupY = 8;

	UINT mNumAmplificationGroupsX = 0;
	UINT mNumAmplificationGroupsY = 0;

	Matrix mWorld = Matrix::Identity;

	std::vector<DirectX::BoundingBox> mGroupBounds;
	std::vector<Vector2> mQuadPatchBoundsY;
	std::vector<float> mHeightmap;

	std::vector<Material*> mLayerMaterials;

	float mSkirtOffsetY = 2.0f;
	float mMaxTess = 6.0f;
	float mMinTessDist = 50;
	float mMaxTessDist = 250;
};

