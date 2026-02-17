
#include "Shaders/Common.hlsl"

#ifndef IS_SHADOW_PASS
#define IS_SHADOW_PASS 0 
#endif

struct VertexOut
{
	float3 PosW     : POSITION;
	float2 TexC     : TEXCOORD0;
	float2 BoundsY  : TEXCOORD1;
};

VertexOut VS(float4 vin : POSITION)
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

	vout.PosW     = mul(float4(posL, 1.0f), gTerrainWorld).xyz;
	vout.TexC     = texC;
	vout.BoundsY  = vin.zw;

	return vout;
}

float CalcTessFactor(float3 p)
{
	float d = distance(p, gEyePosW);

	// max norm in xz plane (useful to see detail levels from a bird's eye).
	//float d = max( abs(p.x-gEyePosW.x), abs(p.z-gEyePosW.z) );

	float s = saturate((d - gTerrainMinTessDist) / (gTerrainMaxTessDist - gTerrainMinTessDist));

	return pow(2, (lerp(gTerrainMaxTess, gTerrainMinTess, s)));
}

struct PatchTess
{
	float EdgeTess[4]   : SV_TessFactor;
	float InsideTess[2] : SV_InsideTessFactor;
};

PatchTess ConstantHS(InputPatch<VertexOut, 4> patch, uint patchID : SV_PrimitiveID)
{
	PatchTess pt;

	//
	// Frustum cull
	//

	// We store the patch BoundsY in the first control point.
	float minY = patch[0].BoundsY.x;
	float maxY = patch[0].BoundsY.y;

	// Build axis-aligned bounding box.  patch[2] is lower-left corner
	// and patch[1] is upper-right corner.
	float3 vMin = float3(patch[2].PosW.x, minY, patch[2].PosW.z);
	float3 vMax = float3(patch[1].PosW.x, maxY, patch[1].PosW.z);

	float3 boxCenter  = 0.5f*(vMin + vMax);

	// Inflate box a bit to compensate for material layer displacement mapping which we did not account for
	// when we computed the patch bounds.
	float3 boxExtents = 0.5f*(vMax - vMin) + float3(1, 1, 1);

	if(AabbOutsideFrustumTest(boxCenter, boxExtents, gWorldFrustumPlanes))
	{
		pt.EdgeTess[0] = 0.0f;
		pt.EdgeTess[1] = 0.0f;
		pt.EdgeTess[2] = 0.0f;
		pt.EdgeTess[3] = 0.0f;

		pt.InsideTess[0] = 0.0f;
		pt.InsideTess[1] = 0.0f;

		return pt;
	}
	//
	// Do normal tessellation based on distance.
	//
	else
	{
		// It is important to do the tess factor calculation based on the
		// edge properties so that edges shared by more than one patch will
		// have the same tessellation factor.  Otherwise, gaps can appear.

		// Compute midpoint on edges, and patch center
		float3 e0 = 0.5f*(patch[0].PosW + patch[2].PosW);
		float3 e1 = 0.5f*(patch[0].PosW + patch[1].PosW);
		float3 e2 = 0.5f*(patch[1].PosW + patch[3].PosW);
		float3 e3 = 0.5f*(patch[2].PosW + patch[3].PosW);
		float3  c = 0.25f*(patch[0].PosW + patch[1].PosW + patch[2].PosW + patch[3].PosW);

		pt.EdgeTess[0] = CalcTessFactor(e0);
		pt.EdgeTess[1] = CalcTessFactor(e1);
		pt.EdgeTess[2] = CalcTessFactor(e2);
		pt.EdgeTess[3] = CalcTessFactor(e3);

		pt.InsideTess[0] = CalcTessFactor(c);
		pt.InsideTess[1] = pt.InsideTess[0];

		return pt;
	}
}

struct HullOut
{
	float3 PosW     : POSITION;
	float2 TexC     : TEXCOORD0;
};

[domain("quad")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("ConstantHS")]
[maxtessfactor(64.0f)]
HullOut HS(InputPatch<VertexOut, 4> p,
		   uint i : SV_OutputControlPointID,
		   uint patchId : SV_PrimitiveID)
{
	HullOut hout;

	// Pass through shader.
	hout.PosW = p[i].PosW;
	hout.TexC = p[i].TexC;

	return hout;
}

struct DomainOut
{
	float4 PosH : SV_POSITION;
	float3 PosW : POSITION0;
	float2 TexC : TEXCOORD0;

#if !IS_SHADOW_PASS
	float4 ShadowPosH : POSITION1;
#endif
};

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

// The domain shader is called for every vertex created by the tessellator.  
// It is like the vertex shader after tessellation.
[domain("quad")]
DomainOut DS(PatchTess patchTess,
			 float2 uv : SV_DomainLocation,
			 const OutputPatch<HullOut, 4> quad)
{
	DomainOut dout;

	// Bilinear interpolation.
	dout.PosW = lerp(
		lerp(quad[0].PosW, quad[1].PosW, uv.x),
		lerp(quad[2].PosW, quad[3].PosW, uv.x),
		uv.y);

	dout.TexC = lerp(
		lerp(quad[0].TexC, quad[1].TexC, uv.x),
		lerp(quad[2].TexC, quad[3].TexC, uv.x),
		uv.y);

	//
	// Displacement mapping
	//
	if( gUseTerrainHeightMap )
	{
		Texture2D heightMap = ResourceDescriptorHeap[gHeightMapSrvIndex];
		dout.PosW.y = heightMap.SampleLevel(GetLinearClampSampler(), dout.TexC, 0).r;
	}

	if( gUseMaterialHeightMaps )
	{
		float materialDisplacement = CalcBlendedDisplacement(dout.TexC);
		dout.PosW.y += materialDisplacement;
	}

	// NOTE: We tried computing the normal in the shader using finite difference, 
	// but the vertices move continuously with fractional_even which creates
	// noticable light shimmering artifacts as the normal changes.  Therefore,
	// we moved the calculation to the pixel shader.  

	// Project to homogeneous clip space.
	dout.PosH = mul(float4(dout.PosW, 1.0f), gViewProj);

#if !IS_SHADOW_PASS
	dout.ShadowPosH = mul(float4(dout.PosW, 1.0f), gShadowTransform);
#endif

	return dout;
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



void CalcBlendedMaterialData(float2 texC, 
    out float4 outAlbedo,
	out float3 outNormalSample,
	out float3 outFresnelR0, 
	out float3 outGlossHeightAo)
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

	// Accumulate the layers.
	for(int i = 0; i < gNumTerrainLayers; ++i)
	{
		// cbuffer packs 8 material indices into 2 uints:
		// uint4 gTerrainLayerMaterialIndices[2];
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

		// Blend each material component.
		outAlbedo = lerp(outAlbedo, diffuseAlbedo, blendPercents[i]);
		outNormalSample = lerp(outNormalSample, normalMapSample, blendPercents[i]);
		outFresnelR0 = lerp(outFresnelR0, fresnelR0, blendPercents[i]);
		outGlossHeightAo = lerp(outGlossHeightAo, glossHeightAo, blendPercents[i]);
	}
}

float4 PS(DomainOut pin) : SV_Target
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