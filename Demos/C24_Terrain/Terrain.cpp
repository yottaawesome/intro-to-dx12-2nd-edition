
#include "Terrain.h"
#include "Effects.h"
#include "FrameResource.h"
#include <fstream>
#include <sstream>

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace DirectX::PackedVector;

Terrain::Terrain(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, const InitInfo& initInfo) :
	md3dDevice(device),
	mInfo(initInfo)
{
	// Divide heightmap into patches such that each patch has CellsPerPatch.
	mNumPatchVertRows = ((mInfo.HeightmapHeight-1) / CellsPerPatch) + 1;
	mNumPatchVertCols = ((mInfo.HeightmapWidth-1) / CellsPerPatch) + 1;

	mNumPatchVertices  = mNumPatchVertRows*mNumPatchVertCols;
	mNumPatchQuadFaces = (mNumPatchVertRows-1)*(mNumPatchVertCols-1);

	LoadHeightmapRaw16();
	CalcAllPatchBoundsY();

	BuildQuadPatchVB(uploadBatch);
	BuildQuadPatchIB(uploadBatch);
	BuildHeightMapTexture(uploadBatch);
}

Terrain::~Terrain()
{
}

void Terrain::BuildDescriptors()
{
	CbvSrvUavHeap& heap = CbvSrvUavHeap::Get();
	mHeightMapSrvIndex = heap.NextFreeIndex();

	CreateSrv2d(md3dDevice, mHeightMapTexture.Get(), DXGI_FORMAT_R32_FLOAT, 1, heap.CpuHandle(mHeightMapSrvIndex));
}

void Terrain::SetMaterialLayers(std::initializer_list<Material*> layers, UINT blendMap0SrvIndex, UINT blendMap1SrvIndex)
{
	assert(layers.size() <= MaxTerrainLayers);
	mLayerMaterials = layers;

	mBlendMap0SrvIndex = blendMap0SrvIndex;
	mBlendMap1SrvIndex = blendMap1SrvIndex;
}

void Terrain::SetMaxTess(float maxTess)
{
	mMaxTess = MathHelper::Clamp(maxTess, 0.0f, 6.0f);
}

void Terrain::SetMinTessDist(float value)
{
	mMinTessDist = value;
}

void Terrain::SetMaxTessDist(float value)
{
	mMaxTessDist = value;
}

void Terrain::SetUseTerrainHeightMap(bool value)
{
	mUseTerrainHeightMap = value;
}

void Terrain::SetUseMaterialHeightMaps(bool value)
{
	mUseMaterialHeightMaps = value;
}

float Terrain::GetWidth()const
{
	// Total terrain width.
	return (mInfo.HeightmapWidth-1)*mInfo.CellSpacing;
}

float Terrain::GetDepth()const
{
	// Total terrain depth.
	return (mInfo.HeightmapHeight-1)*mInfo.CellSpacing;
}

float Terrain::GetHeight(float x, float z)const
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

XMFLOAT4X4 Terrain::GetWorld()const
{
	return mWorld;
}

void Terrain::SetWorld(const XMFLOAT4X4& W)
{
	mWorld = W;
}

void Terrain::Draw(ID3D12GraphicsCommandList* cmdList, ID3D12PipelineState* drawTerrainPso)
{
	cmdList->SetPipelineState(drawTerrainPso);

	D3D12_INDEX_BUFFER_VIEW ibv;
	ibv.BufferLocation = mQuadPatchIB->GetGPUVirtualAddress();
	ibv.Format = DXGI_FORMAT_R32_UINT;
	ibv.SizeInBytes = mNumPatchQuadFaces * 4 * sizeof(std::uint32_t);

	D3D12_VERTEX_BUFFER_VIEW vbv;
	vbv.BufferLocation = mQuadPatchVB->GetGPUVirtualAddress();
	vbv.StrideInBytes = sizeof(XMFLOAT4);
	vbv.SizeInBytes = mNumPatchVertRows*mNumPatchVertCols * sizeof(XMFLOAT4);

	cmdList->IASetVertexBuffers(0, 1, &vbv);
	cmdList->IASetIndexBuffer(&ibv);
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);

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

	drawCB.gUseTerrainHeightMap = mUseTerrainHeightMap ? 1 : 0;
	drawCB.gUseMaterialHeightMaps = mUseMaterialHeightMaps ? 1 : 0;

	GraphicsMemory& linearAllocator = GraphicsMemory::Get(md3dDevice);
	mDrawConstants = linearAllocator.AllocateConstant(drawCB);
	cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, mDrawConstants.GpuAddress());

	cmdList->DrawIndexedInstanced(mNumPatchQuadFaces * 4, 1, 0, 0, 0);
}

void Terrain::LoadHeightmapRaw16()
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

void Terrain::CalcAllPatchBoundsY()
{
	mPatchBoundsY.resize(mNumPatchQuadFaces);

	// For each patch
	for(UINT i = 0; i < mNumPatchVertRows-1; ++i)
	{
		for(UINT j = 0; j < mNumPatchVertCols-1; ++j)
		{
			CalcPatchBoundsY(i, j);
		}
	}
}

void Terrain::CalcPatchBoundsY(UINT i, UINT j)
{
	// Scan the heightmap values this patch covers and compute the min/max height.

	UINT x0 = j*CellsPerPatch;
	UINT x1 = (j+1)*CellsPerPatch;

	UINT y0 = i*CellsPerPatch;
	UINT y1 = (i+1)*CellsPerPatch;

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
	mPatchBoundsY[patchID] = XMFLOAT2(minY, maxY);
}

void Terrain::BuildQuadPatchVB(DirectX::ResourceUploadBatch& uploadBatch)
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
			patchVertices[i*mNumPatchVertCols+j].z = mPatchBoundsY[patchID].x;
			patchVertices[i*mNumPatchVertCols+j].w = mPatchBoundsY[patchID].y;
		}
	}

	CreateStaticBuffer(md3dDevice, uploadBatch,
					   patchVertices.data(), patchVertices.size(), sizeof(XMFLOAT4),
					   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, mQuadPatchVB.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_NONE);
}

void Terrain::BuildQuadPatchIB(DirectX::ResourceUploadBatch& uploadBatch)
{
	std::vector<UINT> indices(mNumPatchQuadFaces*4); // 4 indices per quad face

	// Iterate over each quad and compute indices.
	int k = 0;
	for(UINT i = 0; i < mNumPatchVertRows-1; ++i)
	{
		for(UINT j = 0; j < mNumPatchVertCols-1; ++j)
		{
			// Top row of 2x2 quad patch
			indices[k]   = i*mNumPatchVertCols+j;
			indices[k+1] = i*mNumPatchVertCols+j+1;

			// Bottom row of 2x2 quad patch
			indices[k+2] = (i+1)*mNumPatchVertCols+j;
			indices[k+3] = (i+1)*mNumPatchVertCols+j+1;

			k += 4; // next quad
		}
	}

	CreateStaticBuffer(md3dDevice, uploadBatch,
					   indices.data(), indices.size(), sizeof(UINT),
					   D3D12_RESOURCE_STATE_INDEX_BUFFER, mQuadPatchIB.GetAddressOf(),
					   D3D12_RESOURCE_FLAG_NONE);
}

void Terrain::BuildHeightMapTexture(DirectX::ResourceUploadBatch& uploadBatch)
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
