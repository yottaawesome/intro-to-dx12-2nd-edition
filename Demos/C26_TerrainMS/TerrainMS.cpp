//***************************************************************************************
// TerrainMS.cpp by Frank Luna (C) 2022 All Rights Reserved.
//***************************************************************************************

#include "TerrainMS.h"
#include "FrameResource.h"
#include <fstream>
#include <sstream>

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace DirectX::PackedVector;

TerrainMS::TerrainMS(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, const InitInfo& initInfo) :
	md3dDevice(device),
	mInfo(initInfo)
{
	// Divide heightmap into patches such that each patch has CellsPerQuadPatch.
	mNumPatchVertRows = ((mInfo.HeightmapHeight-1) / CellsPerQuadPatch) + 1;
	mNumPatchVertCols = ((mInfo.HeightmapWidth-1) / CellsPerQuadPatch) + 1;

	assert(mNumPatchVertRows == mNumPatchVertCols);

	const uint32_t numPatchQuadsX = mNumPatchVertCols-1;
	const uint32_t numPatchQuadsY = mNumPatchVertRows-1;
	mNumAmplificationGroupsX = (numPatchQuadsX + mNumQuadsPerGroupX-1) / mNumQuadsPerGroupX;
	mNumAmplificationGroupsY = (numPatchQuadsY + mNumQuadsPerGroupY-1) / mNumQuadsPerGroupY;

	// Shader does not have proper bounds checking if the number of patches 
	// is not evenly divisible by the thread group count.
	assert(numPatchQuadsX % mNumQuadsPerGroupX == 0);
	assert(numPatchQuadsY % mNumQuadsPerGroupY == 0);

	mNumPatchVertices  = mNumPatchVertRows*mNumPatchVertCols;
	mNumPatchQuadFaces = (mNumPatchVertRows-1)*(mNumPatchVertCols-1);

	LoadHeightmapRaw16();
	CalcAllQuadGroupBounds();
	CalcAllQuadPatchBoundsY();
	BuildQuadPatchVB(uploadBatch);
	BuildQuadGroupBoundsBuffer(uploadBatch);
	BuildHeightMapTexture(uploadBatch);
}

TerrainMS::~TerrainMS()
{
}

void TerrainMS::BuildDescriptors()
{
	CbvSrvUavHeap& heap = CbvSrvUavHeap::Get();
	
	mHeightMapSrvIndex = heap.NextFreeIndex();
	mTerrainVerticesSrvIndex = heap.NextFreeIndex();
	mTerrainGroupBoundsSrvIndex = heap.NextFreeIndex();

	CreateSrv2d(md3dDevice, mHeightMapTexture.Get(), DXGI_FORMAT_R32_FLOAT, 1, heap.CpuHandle(mHeightMapSrvIndex));

	CreateBufferSrv(md3dDevice, 0, mNumPatchVertRows*mNumPatchVertCols, sizeof(XMFLOAT4), mQuadPatchVB.Get(), heap.CpuHandle(mTerrainVerticesSrvIndex));
	CreateBufferSrv(md3dDevice, 0, mGroupBounds.size(), sizeof(BoundingBox), mQuadGroupBoundsBuffer.Get(), heap.CpuHandle(mTerrainGroupBoundsSrvIndex));
}

void TerrainMS::SetMaterialLayers(std::initializer_list<Material*> layers, UINT blendMap0SrvIndex, UINT blendMap1SrvIndex)
{
	assert(layers.size() <= MaxTerrainLayers);
	mLayerMaterials = layers;

	mBlendMap0SrvIndex = blendMap0SrvIndex;
	mBlendMap1SrvIndex = blendMap1SrvIndex;
}

void TerrainMS::SetSkirtOffsetY(float value)
{
	mSkirtOffsetY = value;
}

void TerrainMS::SetMinTessDist(float value)
{
	mMinTessDist = value;
}

void TerrainMS::SetMaxTessDist(float value)
{
	mMaxTessDist = value;
}

void TerrainMS::SetMaxTess(float maxTess)
{
	mMaxTess = maxTess;
}

float TerrainMS::GetWidth()const
{
	// Total terrain width.
	return (mInfo.HeightmapWidth-1)*mInfo.CellSpacing;
}

float TerrainMS::GetDepth()const
{
	// Total terrain depth.
	return (mInfo.HeightmapHeight-1)*mInfo.CellSpacing;
}

float TerrainMS::GetHeight(float x, float z)const
{
	// Transform from terrain local space to "cell" space.
	float c = (x + 0.5f*GetWidth()) /  mInfo.CellSpacing;
	float d = (z - 0.5f*GetDepth()) / -mInfo.CellSpacing;

	// Get the row and column we are in.
	int row = (int)floorf(d);
	int col = (int)floorf(c);

	// Grab the heights of the cell we are in.
	// A*--*B
	//  | /|
	//  |/ |
	// C*--*D
	float A = mHeightmap[row*mInfo.HeightmapWidth + col];
	float B = mHeightmap[row*mInfo.HeightmapWidth + col + 1];
	float C = mHeightmap[(row+1)*mInfo.HeightmapWidth + col];
	float D = mHeightmap[(row+1)*mInfo.HeightmapWidth + col + 1];

	// Where we are relative to the cell.
	float s = c - (float)col;
	float t = d - (float)row;

	// If upper triangle ABC.
	if( s + t <= 1.0f)
	{
		float uy = B - A;
		float vy = C - A;
		return A + s*uy + t*vy;
	}
	else // lower triangle DCB.
	{
		float uy = C - D;
		float vy = B - D;
		return D + (1.0f-s)*uy + (1.0f-t)*vy;
	}
}

Matrix TerrainMS::GetWorld()const
{
	return mWorld;
}

void TerrainMS::SetWorld(const Matrix& W)
{
	mWorld = W;
}

void TerrainMS::Draw(ID3D12GraphicsCommandList6* cmdList, 
					 ID3D12PipelineState* drawTerrainPso,
					 ID3D12PipelineState* drawTerrainSkirtPso,
					 bool drawSkirts)
{

	PerTerrainCB drawCB;
	drawCB.gTerrainWorld = mWorld;

	UINT matIndices[MaxTerrainLayers] = { 0 };

	for(UINT i = 0; i < mInfo.NumLayers; ++i)
	{
		matIndices[i] = mLayerMaterials[i]->MatIndex;
	}

	drawCB.gTerrainLayerMaterialIndices[0] = XMUINT4(matIndices[0], matIndices[1], matIndices[2], matIndices[3]);
	drawCB.gTerrainLayerMaterialIndices[1] = XMUINT4(matIndices[4], matIndices[5], matIndices[6], matIndices[7]);


	drawCB.gTerrainWorldCellSpacing = Vector2(mInfo.CellSpacing);
	drawCB.gTerrainWorldSize = Vector2(GetWidth(), GetDepth());

	drawCB.gTerrainHeightMapSize.x = static_cast<float>(mInfo.HeightmapWidth);
	drawCB.gTerrainHeightMapSize.y = static_cast<float>(mInfo.HeightmapHeight);

	drawCB.gTerrainTexelSizeUV.x = 1.0f / static_cast<float>(mInfo.HeightmapWidth);
	drawCB.gTerrainTexelSizeUV.y = 1.0f / static_cast<float>(mInfo.HeightmapHeight);

	drawCB.gTerrainMinTessDist = mMinTessDist;
	drawCB.gTerrainMaxTessDist = mMaxTessDist;
	drawCB.gTerrainMinTess = 0.0f;
	drawCB.gTerrainMaxTess = mMaxTess;

	drawCB.gBlendMap0SrvIndex = mBlendMap0SrvIndex;
	drawCB.gBlendMap1SrvIndex = mBlendMap1SrvIndex;
	drawCB.gHeightMapSrvIndex = mHeightMapSrvIndex;
	drawCB.gNumTerrainLayers = mInfo.NumLayers;

	// Assume square.
	assert(mNumPatchVertRows == mNumPatchVertCols);
	drawCB.gNumQuadVertsPerTerrainSide = mNumPatchVertCols;
	
	drawCB.gTerrainVerticesSrvIndex = mTerrainVerticesSrvIndex;
	drawCB.gTerrainGroupBoundsSrvIndex = mTerrainGroupBoundsSrvIndex;

	drawCB.gNumAmplificationGroupsX = mNumAmplificationGroupsX;
	drawCB.gNumAmplificationGroupsY = mNumAmplificationGroupsY;

	drawCB.gSkirtOffsetY = mSkirtOffsetY;

	GraphicsMemory& linearAllocator = GraphicsMemory::Get(md3dDevice);
	mDrawConstants = linearAllocator.AllocateConstant(drawCB);
	cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, mDrawConstants.GpuAddress());

	cmdList->SetPipelineState(drawTerrainPso);
	cmdList->DispatchMesh(mNumAmplificationGroupsX, mNumAmplificationGroupsY, 1);

	if(drawSkirts)
	{
		cmdList->SetPipelineState(drawTerrainSkirtPso);
		cmdList->DispatchMesh(mNumAmplificationGroupsX, mNumAmplificationGroupsY, 1);
	}
}

void TerrainMS::LoadHeightmapRaw16()
{
	// A 16-bit height for each vertex
	std::vector<uint16_t> in( mInfo.HeightmapWidth * mInfo.HeightmapHeight );

	// Open the file.
	std::filesystem::path absolutePath = std::filesystem::absolute(mInfo.HeightMapFilename);
	std::ifstream inFile;
	inFile.open(absolutePath, std::ios_base::binary);

	if(inFile)
	{
		// Read the RAW bytes.
		inFile.read((char*)&in[0], (std::streamsize)(in.size() * sizeof(uint16_t)));

		// Done with file.
		inFile.close();
	}

	constexpr float MaxUShort = static_cast<float>(std::numeric_limits<uint16_t>::max());

	// Copy the array data into a float array and scale it.
	mHeightmap.resize( in.size(), 0);
	for(UINT i = 0; i < in.size(); ++i)
	{ 
		float heightUnorm = in[i] / MaxUShort;
		mHeightmap[i] = mInfo.HeightScale * heightUnorm + mInfo.HeightOffset;
	}
}

void TerrainMS::CalcAllQuadGroupBounds()
{
	mGroupBounds.resize(mNumAmplificationGroupsX * mNumAmplificationGroupsY);

	// Computing a bounding box around each group of quad patches for amplification shader culling.
	for(UINT groupY = 0; groupY < mNumAmplificationGroupsY; ++groupY)
	{
		for(UINT groupX = 0; groupX < mNumAmplificationGroupsX; ++groupX)
		{
			BoundingBox groupBox = CalcQuadGroupBounds(groupX, groupY);
			mGroupBounds[groupY * mNumAmplificationGroupsY + groupX] = groupBox;
		}
	}
}

BoundingBox TerrainMS::CalcQuadGroupBounds(UINT groupX, UINT groupY)
{
	float halfWidth = 0.5f*GetWidth();
	float halfDepth = 0.5f*GetDepth();

	float groupWidth = GetWidth() / mNumAmplificationGroupsX;
	float groupDepth = GetDepth() / mNumAmplificationGroupsY;

	// Scan the heightmap values this patch covers and compute the min/max height.

	UINT x0 = groupX * mNumQuadsPerGroupX * CellsPerQuadPatch;
	UINT x1 = (groupX+1) * mNumQuadsPerGroupX * CellsPerQuadPatch;

	UINT y0 = groupY * mNumQuadsPerGroupY * CellsPerQuadPatch;
	UINT y1 = (groupY+1) * mNumQuadsPerGroupY * CellsPerQuadPatch;

	float minY = +MathHelper::Infinity;
	float maxY = -MathHelper::Infinity;
	for(UINT y = y0; y <= y1; ++y)
	{
		for(UINT x = x0; x <= x1; ++x)
		{
			UINT k = y*mInfo.HeightmapWidth + x;
			minY = MathHelper::Min(minY, mHeightmap[k]);
			maxY = MathHelper::Max(maxY, mHeightmap[k]);
		}
	}

	float groupCenterX = -halfWidth + groupX*groupWidth + 0.5f * groupWidth;
	float groupCenterZ =  halfDepth - groupY*groupDepth - 0.5f * groupDepth;
	float groupCenterY = 0.5f * (minY + maxY);

	float extentX = 0.5f * groupWidth;
	float extentY = 0.5f * (maxY - minY);
	float extentZ = 0.5f * groupDepth;

	BoundingBox box;
	box.Center = XMFLOAT3(groupCenterX, groupCenterY, groupCenterZ);
	box.Extents = XMFLOAT3(extentX, extentY, extentZ);

	return box;
}

void TerrainMS::CalcAllQuadPatchBoundsY()
{
	mQuadPatchBoundsY.resize(mNumPatchQuadFaces);
	// For each patch
	for(UINT i = 0; i < mNumPatchVertRows-1; ++i)
	{
		for(UINT j = 0; j < mNumPatchVertCols-1; ++j)
		{
			CalcQuadPatchBoundsY(i, j);
		}
	}
}
void TerrainMS::CalcQuadPatchBoundsY(UINT i, UINT j)
{
	// Scan the heightmap values this patch covers and compute the min/max height.
	UINT x0 = j*CellsPerQuadPatch;
	UINT x1 = (j+1)*CellsPerQuadPatch;
	UINT y0 = i*CellsPerQuadPatch;
	UINT y1 = (i+1)*CellsPerQuadPatch;

	float minY = +MathHelper::Infinity;
	float maxY = -MathHelper::Infinity;
	for(UINT y = y0; y <= y1; ++y)
	{
		for(UINT x = x0; x <= x1; ++x)
		{
			UINT k = y*mInfo.HeightmapWidth + x;
			minY = MathHelper::Min(minY, mHeightmap[k]);
			maxY = MathHelper::Max(maxY, mHeightmap[k]);
		}
	}

	UINT patchID = i*(mNumPatchVertCols-1)+j;
	mQuadPatchBoundsY[patchID] = XMFLOAT2(minY, maxY);
}

void TerrainMS::BuildQuadPatchVB(DirectX::ResourceUploadBatch& uploadBatch)
{
	std::vector<XMFLOAT4> patchVertices(mNumPatchVertRows*mNumPatchVertCols);

	float halfWidth = 0.5f*GetWidth();
	float halfDepth = 0.5f*GetDepth();

	float patchWidth = GetWidth() / (mNumPatchVertCols-1);
	float patchDepth = GetDepth() / (mNumPatchVertRows-1);

	for(UINT i = 0; i < mNumPatchVertRows; ++i)
	{
		float z = halfDepth - i*patchDepth;
		for(UINT j = 0; j < mNumPatchVertCols; ++j)
		{
			float x = -halfWidth + j*patchWidth;

			// xy: Patch 2d point position in xz-plane.
			patchVertices[i*mNumPatchVertCols+j] = XMFLOAT4(x, z, 0.0f, 0.0f);
		}
	}

	// Store axis-aligned bounding box y-bounds in upper-left patch corner.
	for(UINT i = 0; i < mNumPatchVertRows-1; ++i)
	{
		for(UINT j = 0; j < mNumPatchVertCols-1; ++j)
		{
			UINT patchID = i*(mNumPatchVertCols-1)+j;

			// zw: Patch axis y-bounds.
			patchVertices[i*mNumPatchVertCols+j].z = mQuadPatchBoundsY[patchID].x;
			patchVertices[i*mNumPatchVertCols+j].w = mQuadPatchBoundsY[patchID].y;
		}
	}


	CreateStaticBuffer(md3dDevice, uploadBatch,
					   patchVertices.data(), patchVertices.size(), sizeof(XMFLOAT4),
					   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, mQuadPatchVB.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_NONE);
}

void TerrainMS::BuildQuadGroupBoundsBuffer(DirectX::ResourceUploadBatch& uploadBatch)
{
	CreateStaticBuffer(md3dDevice, uploadBatch,
					   mGroupBounds.data(), mGroupBounds.size(), sizeof(BoundingBox),
					   D3D12_RESOURCE_STATE_GENERIC_READ, mQuadGroupBoundsBuffer.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_NONE);
}

void TerrainMS::BuildHeightMapTexture(DirectX::ResourceUploadBatch& uploadBatch)
{
	D3D12_SUBRESOURCE_DATA subResourceData = {};
	subResourceData.pData = mHeightmap.data();
	subResourceData.RowPitch = mInfo.HeightmapWidth*sizeof(float);
	subResourceData.SlicePitch = 0;

	ThrowIfFailed(CreateTextureFromMemory(md3dDevice,
				  uploadBatch,
				  mInfo.HeightmapWidth, mInfo.HeightmapHeight,
				  DXGI_FORMAT_R32_FLOAT,
				  subResourceData,
				  &mHeightMapTexture, 
				  false,
				  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
}
