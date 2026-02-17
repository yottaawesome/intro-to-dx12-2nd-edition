
#include "Shaders/Common.hlsl"

#ifndef IS_SHADOW_PASS
#define IS_SHADOW_PASS 0 
#endif

struct VertexOut
{
	float4 PosH : SV_POSITION;
	float3 PosW : POSITION0;
	float2 TexC : TEXCOORD0;
#if !IS_SHADOW_PASS
	float4 ShadowPosH : POSITION1;
#endif
};

uint CalcNumSubdivisions(float3 p)
{
	//float d = distance(p, gEyePosW);

	float d = max( max( abs(p.x-gEyePosW.x), abs(p.y-gEyePosW.y) ), abs(p.z-gEyePosW.z) );

	float s = saturate((d - gTerrainMinTessDist) / (gTerrainMaxTessDist - gTerrainMinTessDist));

	return (uint)(lerp(gTerrainMaxTess, gTerrainMinTess, s) + 0.5f);
}

float CalcBlendedDisplacement(float2 texC)
{
	// Sample the blend map.
	Texture2D blendMap0 = ResourceDescriptorHeap[gBlendMap0SrvIndex];
	Texture2D blendMap1 = ResourceDescriptorHeap[gBlendMap1SrvIndex];
	float4 blend0  = blendMap0.SampleLevel(GetLinearClampSampler(), texC, 0.0f);
	float4 blend1  = blendMap1.SampleLevel(GetLinearClampSampler(), texC, 0.0f);

	float blendPercents[8] = {
		1.0f, blend0.y, blend0.z, blend0.w,
		blend1.x, blend1.y, blend1.z, blend1.w
	};

	float height = 0.0f;

	for(int i = 0; i < gNumTerrainLayers; ++i)
	{
		uint matVectorIndex = i / 4;
		uint matIndexInVector = i % 4;
		uint matIndex = gTerrainLayerMaterialIndices[matVectorIndex][matIndexInVector];

		// Fetch the material data.
		MaterialData matData = gMaterialData[matIndex];
		uint glossHeightAoMapIndex = matData.GlossHeightAoMapIndex;

		float2 layerTexC = mul(float4(texC, 0.0f, 1.0f), matData.MatTransform).xy;

		// Dynamically look up the texture in the array.
		Texture2D glossHeightAoMap = ResourceDescriptorHeap[glossHeightAoMapIndex];
		float layerHeight = glossHeightAoMap.SampleLevel(GetAnisoWrapSampler(), layerTexC, 0.0f).g;

		layerHeight *= matData.DisplacementScale;

		height = lerp(height, layerHeight, blendPercents[i]);
	}

	return height;
}

VertexOut ProcessVertexWithOffset(float2 vin, float offsetY)
{
	VertexOut vout;

	float2 bottomLeft = -0.5f*gTerrainWorldSize;

	float3 posL = float3(vin.x, 0.0f, vin.y);

	float2 texC = (posL.xz - bottomLeft) / gTerrainWorldSize;
	texC.y = 1.0f - texC.y;

	// Remap [0,1]-->[halfTexel, 1.0f - halfTexel] so that vertices coincide with
	// texel centers. Texel centers are offset half a texel from the top-left
	// corner of the texel. 
	float2 halfTexel = 0.5f * gTerrainTexelSizeUV;
	texC = halfTexel + texC * (1.0f - 2*halfTexel);

	// Displace the patch corners to world space.  This is to make 
	// the eye to patch distance calculation more accurate.
	Texture2D heightMap = ResourceDescriptorHeap[gHeightMapSrvIndex];
	posL.y = heightMap.SampleLevel(GetLinearClampSampler(), texC, 0).r;

	posL.y += offsetY;

	vout.PosW = mul(float4(posL, 1.0f), gTerrainWorld).xyz;
	vout.TexC = texC;

	float materialDisplacement = CalcBlendedDisplacement(texC);
	vout.PosW.y += materialDisplacement;

	// Project to homogeneous clip space.
	vout.PosH = mul(float4(vout.PosW, 1.0f), gViewProj);

#if !IS_SHADOW_PASS
	vout.ShadowPosH = mul(float4(vout.PosW, 1.0f), gShadowTransform);
#endif

	return vout;
}

VertexOut ProcessVertex(float2 vin)
{
	const float offsetY = 0.0f;
	return ProcessVertexWithOffset(vin, offsetY);
}

// Amplification payload can be up to 16KB
struct PatchPayload
{
	uint NumMeshletsPerQuadPatchSide;
	uint MeshletCellsPerSide;
	float MeshletSizeInUVs;
	float CellSpacingInUVs;

	// Maps thread group index k in 8x8 to global quad patch index
	// relative to entire terrain.
	uint4 QuadPatchIndex[64];
};

struct SkirtPayload
{
	uint QuadPatchCellsPerSide;
	uint4 QuadPatchIndex[64];
};

struct Bounds
{
	float3 Center;
	float3 Extents;
};

// This is basically a limitation from the skirt implementation. We want one meshlet to be able to process
// the entire side of a quad, and we are limited to 256 vertices and 256 primitives per mesh shader, but we still
// want the number of threads on the thread group to be a multiple of 64.
#define MAX_QUAD_CELLS_PER_SIDE 64

#define MAX_MS_CELLS_PER_SIDE 8
#define MAX_MS_TRIS (MAX_MS_CELLS_PER_SIDE * MAX_MS_CELLS_PER_SIDE * 2)
#define MAX_MS_VERTS ( (MAX_MS_CELLS_PER_SIDE+1) * (MAX_MS_CELLS_PER_SIDE+1) )

groupshared PatchPayload gPayload;
groupshared SkirtPayload gSkirtPayload;

//
// Each thread processes a quad patch.
//
[numthreads(8, 8, 1)]
void TerrainAS(
	uint3 groupId : SV_GroupID,
	uint3 groupThreadId : SV_GroupThreadID,
	uint3 dispatchThreadID : SV_DispatchThreadID)
{
	// Only set to nonzero if it passes frustum culling.
	uint dispatchX = 0;
	uint dispatchY = 0;
	uint dispatchZ = 0;

	StructuredBuffer<Bounds> groupBoundsBuffer = ResourceDescriptorHeap[gTerrainGroupBoundsSrvIndex];
	const Bounds groupBounds = groupBoundsBuffer[groupId.y * gNumAmplificationGroupsX + groupId.x];

	const float3 vMinL = groupBounds.Center - groupBounds.Extents;
	const float3 vMaxL = groupBounds.Center + groupBounds.Extents;

	// Assumes no rotation.
	const float3 vMinW = mul(float4(vMinL, 1.0f), gTerrainWorld).xyz;
	const float3 vMaxW = mul(float4(vMaxL, 1.0f), gTerrainWorld).xyz;
	const float3 boxCenter = 0.5f*(vMinW + vMaxW);

	// Inflate box a bit to compensate for material layer displacement mapping which we did not account for
	// when we computed the patch bounds.
	const float3 boxExtents = 0.5f*(vMaxW - vMinW) + float3(1, 1, 1);

	// Cull at group granularity.
	if(!AabbOutsideFrustumTest(boxCenter, boxExtents, gWorldFrustumPlanes))
	{
		const uint numPatchSubdivisions = CalcNumSubdivisions(boxCenter);

		// For each quad patch in the group, store the global quad patch index across all groups.
		gPayload.QuadPatchIndex[groupThreadId.y*8+groupThreadId.x].xy = dispatchThreadID.xy;

		const uint patchCellsPerSide = min((1u << numPatchSubdivisions), MAX_QUAD_CELLS_PER_SIDE);

		// Each mesh shader group has 8x8 threads, so based on the required tessellation level, we need
		// to dispatch a grid of mesh shaders per quad patch.
		// Also note we can only output up to 256 vertices and 256 primitives per mesh shader. 
		const uint numMeshletsPerQuadPatchSide = (patchCellsPerSide + (MAX_MS_CELLS_PER_SIDE - 1)) / MAX_MS_CELLS_PER_SIDE;

		gPayload.NumMeshletsPerQuadPatchSide = numMeshletsPerQuadPatchSide;
		gPayload.MeshletCellsPerSide = numMeshletsPerQuadPatchSide > 1 ? MAX_MS_CELLS_PER_SIDE : patchCellsPerSide;

		// Meshlet and cell sizes in UV space [0,1] over the input quad patch.
		gPayload.MeshletSizeInUVs = 1.0f / numMeshletsPerQuadPatchSide;
		gPayload.CellSpacingInUVs = gPayload.MeshletSizeInUVs / gPayload.MeshletCellsPerSide;

		// Each amplification group processes a grid of quad patches, and
		// each quad patch has a grid of meshlets.
		const uint quadPatchesPerSide = 8;
		dispatchX = quadPatchesPerSide * numMeshletsPerQuadPatchSide;
		dispatchY = quadPatchesPerSide * numMeshletsPerQuadPatchSide;
		dispatchZ = 1;
	}

	// Dispatch child mesh shaders groups.
	DispatchMesh(dispatchX, dispatchY, dispatchZ, gPayload);
}

[outputtopology("triangle")]
[numthreads(8, 8, 1)]
void TerrainMS(
	uint3 groupId : SV_GroupID,
	uint3 groupThreadId : SV_GroupThreadID,
	in payload PatchPayload payload,
	out vertices VertexOut outVerts[MAX_MS_VERTS],
	out indices uint3 outIndices[MAX_MS_TRIS])
{
	const uint cellsPerSide = payload.MeshletCellsPerSide;
	const uint vertsPerSide = cellsPerSide + 1;

	const uint numVerts = vertsPerSide * vertsPerSide;
	const uint numCells = cellsPerSide * cellsPerSide;

	SetMeshOutputCounts(numVerts, numCells * 2);

	//
	// Get the quad patch vertices this meshlet came from.
	//
	
	// Figure out which quad patch this meshlet came from. Recall that each amplification 
	// group processes a grid of quad patches, and each quad patch has a grid of meshlets.
	const uint2 localQuadPatchIndex = groupId.xy / payload.NumMeshletsPerQuadPatchSide;

	const uint2 globalQuadPatchIndex = payload.QuadPatchIndex[localQuadPatchIndex.y * 8 + localQuadPatchIndex.x].xy;

	const uint index0 = globalQuadPatchIndex.y * gNumQuadVertsPerTerrainSide + globalQuadPatchIndex.x;
	const uint index1 = globalQuadPatchIndex.y * gNumQuadVertsPerTerrainSide + globalQuadPatchIndex.x + 1;
	const uint index2 = (globalQuadPatchIndex.y+1) * gNumQuadVertsPerTerrainSide + globalQuadPatchIndex.x;
	const uint index3 = (globalQuadPatchIndex.y+1) * gNumQuadVertsPerTerrainSide + globalQuadPatchIndex.x + 1;

	StructuredBuffer<float4> vertexBuffer = ResourceDescriptorHeap[gTerrainVerticesSrvIndex];
	const float2 v0 = vertexBuffer[index0].xy;
	const float2 v1 = vertexBuffer[index1].xy;
	const float2 v2 = vertexBuffer[index2].xy;
	const float2 v3 = vertexBuffer[index3].xy;

	//
	// Process cell vertices. We have a thread per cell, but we have Square(cellsPerSide + 1)
	// many vertices. So the last row/column of threads has to process two vertices.
	//

	const uint cellX = groupThreadId.x;
	const uint cellY = groupThreadId.y; 

	// Note: May not utilize the whole group if cellsPerSide < [numthreads(8, 8, 1)].
	if( cellX < cellsPerSide && cellY < cellsPerSide )
	{
		const float meshletSizeInUVs = payload.MeshletSizeInUVs;
		const float cellSpacingInUVs = payload.CellSpacingInUVs;

		// Meshlet index relative to the quad patch it comes from.
		const uint2 meshletIndex = groupId.xy % payload.NumMeshletsPerQuadPatchSide;

		// Offset inside the patch for this meshlet.
		const float patchOffsetU = meshletIndex.x * meshletSizeInUVs;
		const float patchOffsetV = meshletIndex.y * meshletSizeInUVs;

		// Offset to the vertex in meshlet relative to the patch.
		const float vertU = patchOffsetU + cellX * cellSpacingInUVs;
		const float vertV = patchOffsetV + cellY * cellSpacingInUVs;

		// Bilinear interpolation.
		float2 vertPos = lerp(
			lerp(v0.xy, v1.xy, vertU),
			lerp(v2.xy, v3.xy, vertU),
			vertV); 

		const uint vindex = cellY * vertsPerSide + cellX;
		outVerts[vindex] = ProcessVertex( vertPos );

		// Some threads need to process an extra vertex to handle right/bottom edge.
		// Remember, a quad patch can be covered by several meshlets.
		if(cellX == (cellsPerSide-1)) // right edge
		{
			const uint nextVertIndex = vindex + 1;
			float2 nextVert = lerp(
				lerp(v0.xy, v1.xy, vertU + cellSpacingInUVs),
				lerp(v2.xy, v3.xy, vertU + cellSpacingInUVs),
				vertV); 

			outVerts[nextVertIndex] = ProcessVertex( nextVert );
		}

		if(cellY == (cellsPerSide-1)) // bottom edge
		{
			const uint nextVertIndex = (cellY+1) * vertsPerSide + cellX;
			float2 nextVert = lerp(
				lerp(v0.xy, v1.xy, vertU),
				lerp(v2.xy, v3.xy, vertU),
				vertV + cellSpacingInUVs); 

			outVerts[nextVertIndex] = ProcessVertex( nextVert );
		}

		if(cellX == 0 && cellY == 0) // bottom-right vertex.
		{
			const uint lastVertIndex = numVerts - 1;

			const float meshletRightEdgeU = patchOffsetU + cellsPerSide * cellSpacingInUVs;
			const float meshletBottomEdgeV = patchOffsetV + cellsPerSide * cellSpacingInUVs;

			float2 lastVert = lerp(
				lerp(v0.xy, v1.xy, meshletRightEdgeU),
				lerp(v2.xy, v3.xy, meshletRightEdgeU),
				meshletBottomEdgeV); 

			outVerts[lastVertIndex] = ProcessVertex( lastVert );
		}
	}

	//
	// Process meshlet cell indices. These are relative to outVerts.
	//
	if( cellX < cellsPerSide && cellY < cellsPerSide )
	{
		const uint i0 = cellY * vertsPerSide + cellX;
		const uint i1 = cellY * vertsPerSide + cellX + 1;
		const uint i2 = (cellY+1) * vertsPerSide + cellX;
		const uint i3 = (cellY+1) * vertsPerSide + cellX + 1;

		const uint cellIndex = cellY * cellsPerSide + cellX;
		outIndices[cellIndex*2 + 0] = uint3(i0, i1, i2);
		outIndices[cellIndex*2 + 1] = uint3(i2, i1, i3);
	}
}

//
// Each thread processes a quad patch.
//
[numthreads(8, 8, 1)]
void TerrainSkirtAS(
	uint3 groupId : SV_GroupID,
	uint3 groupThreadId : SV_GroupThreadID,
	uint3 dispatchThreadID : SV_DispatchThreadID)
{
	// Only set to nonzero if it passes frustum culling.
	uint dispatchX = 0;
	uint dispatchY = 0;
	uint dispatchZ = 0;

	StructuredBuffer<Bounds> groupBoundsBuffer = ResourceDescriptorHeap[gTerrainGroupBoundsSrvIndex];
	const Bounds groupBounds = groupBoundsBuffer[groupId.y * gNumAmplificationGroupsX + groupId.x];

	const float3 vMinL = groupBounds.Center - groupBounds.Extents;
	const float3 vMaxL = groupBounds.Center + groupBounds.Extents;

	// Assumes no rotation.
	const float3 vMinW = mul(float4(vMinL, 1.0f), gTerrainWorld).xyz;
	const float3 vMaxW = mul(float4(vMaxL, 1.0f), gTerrainWorld).xyz;
	const float3 boxCenter = 0.5f*(vMinW + vMaxW);

	// Inflate box a bit to compensate for material layer displacement mapping which we did not account for
	// when we computed the patch bounds.
	const float3 boxExtents = 0.5f*(vMaxW - vMinW) + float3(1, 1, 1);

	// Cull at group granularity.
	if(!AabbOutsideFrustumTest(boxCenter, boxExtents, gWorldFrustumPlanes))
	{
		const uint numPatchSubdivisions = CalcNumSubdivisions(boxCenter);

		gSkirtPayload.QuadPatchIndex[groupThreadId.y*8+groupThreadId.x].xy = dispatchThreadID.xy;

		gSkirtPayload.QuadPatchCellsPerSide = min((1u << numPatchSubdivisions), MAX_QUAD_CELLS_PER_SIDE);

		// Dispatch mesh shader for each side of quad.
		const uint edgeCount = 4;
		const uint quadPatchesPerSide = 8;
		dispatchX = quadPatchesPerSide * quadPatchesPerSide * edgeCount;
		dispatchY = 1;
		dispatchZ = 1;
	}

	// Dispatch child mesh shader groups.
	DispatchMesh(dispatchX, dispatchY, dispatchZ, gSkirtPayload);
}

#define MAX_MS_SKIRT_VERTS (2*65)
#define MAX_MS_SKIRT_TRIS (2*64)

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void TerrainSkirtMS(
	uint3 groupId : SV_GroupID,
	uint3 groupThreadId : SV_GroupThreadID,
	in payload SkirtPayload payload,
	out vertices VertexOut outVerts[MAX_MS_SKIRT_VERTS],
	out indices uint3 outIndices[MAX_MS_SKIRT_TRIS])
{
	const uint cellsPerSide = payload.QuadPatchCellsPerSide;
	const uint vertsPerSide = cellsPerSide + 1;

	SetMeshOutputCounts(2*vertsPerSide, cellsPerSide * 2);

	//
	// Get the quad patch vertices this meshlet came from.
	//
	
	// Each quad dispatches 4 meshlets, one for each edge.
	const uint edgeCount = 4;
	const uint localQuadPatchIndex = groupId.x / edgeCount;
	const uint edgeIndex = groupId.x % edgeCount;

	const uint2 globalQuadPatchIndex = payload.QuadPatchIndex[localQuadPatchIndex].xy;

	const uint index0 = globalQuadPatchIndex.y * gNumQuadVertsPerTerrainSide + globalQuadPatchIndex.x;
	const uint index1 = globalQuadPatchIndex.y * gNumQuadVertsPerTerrainSide + globalQuadPatchIndex.x + 1;
	const uint index2 = (globalQuadPatchIndex.y+1) * gNumQuadVertsPerTerrainSide + globalQuadPatchIndex.x;
	const uint index3 = (globalQuadPatchIndex.y+1) * gNumQuadVertsPerTerrainSide + globalQuadPatchIndex.x + 1;

	StructuredBuffer<float4> vertexBuffer = ResourceDescriptorHeap[gTerrainVerticesSrvIndex];
	const float2 v0 = vertexBuffer[index0].xy;
	const float2 v1 = vertexBuffer[index1].xy;
	const float2 v2 = vertexBuffer[index2].xy;
	const float2 v3 = vertexBuffer[index3].xy;

	//
	// Process skirt vertices. Make all the skirt quads "outward facing" or turn off backside culling.
	//

	float2 p;
	float2 q;
	if(edgeIndex == 0) // top
	{
		p = v1;
		q = v0;
	}
	else if(edgeIndex == 1) // right
	{
		p = v3;
		q = v1;
	}
	else if(edgeIndex == 2) // bottom
	{
		p = v2;
		q = v3;
	}
	else if(edgeIndex == 3) // left
	{
		p = v0;
		q = v2;
	}

	const uint cellX = groupThreadId.x;
	const float cellSpacingInUVs = 1.0f / cellsPerSide;
	if( cellX < cellsPerSide )
	{
		float2 vertPos = lerp(p, q, cellX * cellSpacingInUVs);

		const float skirtOffsetY = -gSkirtOffsetY;

		outVerts[cellX] = ProcessVertex( vertPos );
		outVerts[vertsPerSide + cellX] = ProcessVertexWithOffset( vertPos, skirtOffsetY );

		// Last cell needs to process an extra vertex.
		if(cellX == (cellsPerSide-1))
		{
			outVerts[cellX+1] = ProcessVertex( q );
			outVerts[vertsPerSide + cellX+1] = ProcessVertexWithOffset( q, skirtOffsetY );
		}
	}

	//
	// Process meshlet cell indices. These are relative to outVerts.
	//
	if( cellX < cellsPerSide )
	{
		const uint i0 = cellX;
		const uint i1 = cellX + 1;
		const uint i2 = vertsPerSide + cellX;
		const uint i3 = vertsPerSide + cellX + 1;

		outIndices[cellX*2 + 0] = uint3(i0, i1, i2);
		outIndices[cellX*2 + 1] = uint3(i2, i1, i3);
	}
}

#if !IS_SHADOW_PASS

void EstimateTangentFrame(float2 texC, out float3 outTangentW, out float3 outBitangentW, out float3 outNormalW)
{
	//
	// Estimate normal and tangent using central differences.
	//
	float2 leftTexC   = texC + float2(-gTerrainTexelSizeUV.x, 0.0f);
	float2 rightTexC  = texC + float2(+gTerrainTexelSizeUV.x, 0.0f);
	float2 bottomTexC = texC + float2(0.0f, +gTerrainTexelSizeUV.y);
	float2 topTexC    = texC + float2(0.0f, -gTerrainTexelSizeUV.y);

	Texture2D heightMap = ResourceDescriptorHeap[gHeightMapSrvIndex];
	float leftY   = heightMap.SampleLevel(GetLinearClampSampler(), leftTexC, 0).r;
	float rightY  = heightMap.SampleLevel(GetLinearClampSampler(), rightTexC, 0).r;
	float bottomY = heightMap.SampleLevel(GetLinearClampSampler(), bottomTexC, 0).r;
	float topY    = heightMap.SampleLevel(GetLinearClampSampler(), topTexC, 0).r;

	outTangentW = normalize(float3(2.0f*gTerrainWorldCellSpacing.x, rightY - leftY, 0.0f));
	outBitangentW = normalize(float3(0.0f, bottomY - topY, -2.0f*gTerrainWorldCellSpacing.y));
	outNormalW = cross(outTangentW, outBitangentW);
}



void CalcBlendedMaterialData(float2 texC, out float4 outAlbedo, out float3 outNormalSample, out float3 outFresnelR0, out float3 outGlossHeightAo)
{
	outAlbedo = float4(0.0f, 0.0f, 0.0f, 0.0f);
	outNormalSample = float3(0.0f, 0.0f, 0.0f);
	outFresnelR0 = float3(0.0f, 0.0f, 0.0f);
	outGlossHeightAo = float3(0.0f, 0.0f, 0.0f);

	// Sample the blend map.
	Texture2D blendMap0 = ResourceDescriptorHeap[gBlendMap0SrvIndex];
	Texture2D blendMap1 = ResourceDescriptorHeap[gBlendMap1SrvIndex];
	float4 blend0  = blendMap0.SampleLevel(GetLinearClampSampler(), texC, 0.0f);
	float4 blend1  = blendMap1.SampleLevel(GetLinearClampSampler(), texC, 0.0f);

	float blendPercents[8] = {
		1.0f, blend0.y, blend0.z, blend0.w,
		blend1.x, blend1.y, blend1.z, blend1.w
	};

	for(int i = 0; i < gNumTerrainLayers; ++i)
	{
		uint matVectorIndex = i / 4;
		uint matIndexInVector = i % 4;
		uint matIndex = gTerrainLayerMaterialIndices[matVectorIndex][matIndexInVector];

		// Fetch the material data.
		MaterialData matData = gMaterialData[matIndex];
		float4 diffuseAlbedo = matData.DiffuseAlbedo;
		float3 fresnelR0 = matData.FresnelR0;
		float  roughness = matData.Roughness;
		uint diffuseMapIndex = matData.DiffuseMapIndex;
		uint normalMapIndex = matData.NormalMapIndex;
		uint glossHeightAoMapIndex = matData.GlossHeightAoMapIndex;

		float2 layerTexC = mul(float4(texC, 0.0f, 1.0f), matData.MatTransform).xy;

		// Dynamically look up the texture in the array.
		Texture2D diffuseMap = ResourceDescriptorHeap[diffuseMapIndex];
		Texture2D normalMap = ResourceDescriptorHeap[normalMapIndex];
		Texture2D glossHeightAoMap = ResourceDescriptorHeap[glossHeightAoMapIndex];

		diffuseAlbedo *= diffuseMap.Sample(GetAnisoWrapSampler(), layerTexC);
		float3 normalMapSample = normalMap.Sample(GetAnisoWrapSampler(), layerTexC).rgb;
		float3 glossHeightAo = glossHeightAoMap.Sample(GetAnisoWrapSampler(), layerTexC).rgb;

		glossHeightAo.x *= (1.0f - roughness);

		outAlbedo = lerp(outAlbedo, diffuseAlbedo, blendPercents[i]);
		outNormalSample = lerp(outNormalSample, normalMapSample, blendPercents[i]);
		outFresnelR0 = lerp(outFresnelR0, fresnelR0, blendPercents[i]);
		outGlossHeightAo = lerp(outGlossHeightAo, glossHeightAo, blendPercents[i]);
	}
}

float4 PS(VertexOut pin) : SV_Target
{
	float4 diffuseAlbedo;
	float3 normalMapSample;
	float3 fresnelR0;
	float3 glossHeightAo;
	CalcBlendedMaterialData(pin.TexC, diffuseAlbedo, normalMapSample, fresnelR0, glossHeightAo);

	float3 tangentW;
	float3 bitangentW;
	float3 normalW;
	EstimateTangentFrame(pin.TexC, tangentW, bitangentW, normalW);

	float3 bumpedNormalW = normalW;
	if( gNormalMapsEnabled )
	{
		bumpedNormalW = NormalSampleToWorldSpace(normalMapSample, normalW, tangentW);
	}

	// Uncomment to turn off normal mapping.
	//bumpedNormalW = normalW;

	// Vector from point being lit to eye. 
	float3 toEyeW = normalize(gEyePosW - pin.PosW);

	// Light terms.
	float4 ambient = gAmbientLight*diffuseAlbedo;
	ambient *= glossHeightAo.z;

	// Only the first light casts a shadow.
	float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
	if( gShadowsEnabled )
	{
		shadowFactor[0] = CalcShadowFactor(pin.ShadowPosH);
	}

	const float shininess = glossHeightAo.x;
	Material mat = { diffuseAlbedo, fresnelR0, shininess };
	float4 directLight = ComputeLighting(gLights, mat, pin.PosW,
		bumpedNormalW, toEyeW, shadowFactor);

	float4 litColor = ambient + directLight;

	// Add in specular reflections.
	if( gReflectionsEnabled )
	{
		TextureCube gCubeMap = ResourceDescriptorHeap[gSkyBoxIndex];
		float3 r = reflect(-toEyeW, bumpedNormalW);
		float4 reflectionColor = gCubeMap.Sample(GetLinearWrapSampler(), r);
		float3 fresnelFactor = SchlickFresnel(fresnelR0, bumpedNormalW, r);
		litColor.rgb += shininess * fresnelFactor * reflectionColor.rgb;
	}

	// Common convention to take alpha from diffuse albedo.
	litColor.a = diffuseAlbedo.a;

	return litColor;
}

#endif
