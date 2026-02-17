//***************************************************************************************
// Shadows.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

// Include common HLSL code.
#include "Shaders/Common.hlsl"

struct VertexIn
{
	float3 PosL    : POSITION;
	float2 TexC    : TEXCOORD;
#if SKINNED
    float3 BoneWeights : WEIGHTS;
    uint4 BoneIndices  : BONEINDICES;
#endif
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
	float2 TexC    : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
	VertexOut vout = (VertexOut)0.0f;

	MaterialData matData = gMaterialData[gMaterialIndex];
	
#if SKINNED
    ApplySkinningShadows( vin.BoneWeights, vin.BoneIndices, vin.PosL);
#endif

    // Transform to world space.
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);

    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);
	
	// Output vertex attributes for interpolation across triangle.
	float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
	vout.TexC = mul(texC, matData.MatTransform).xy;
	
    return vout;
}

// This is only used for alpha cut out geometry, so that shadows 
// show up correctly.  Geometry that does not need to sample a
// texture can use a NULL pixel shader for depth pass.
void PS(VertexOut pin) 
{
#ifdef ALPHA_TEST
	// Fetch the material data.
	MaterialData matData = gMaterialData[gMaterialIndex];
	float4 diffuseAlbedo = matData.DiffuseAlbedo;
    uint diffuseMapIndex = matData.DiffuseMapIndex;
	
	// Dynamically look up the texture in the array.
	Texture2D diffuseMap = ResourceDescriptorHeap[diffuseMapIndex];
	diffuseAlbedo *= diffuseMap.Sample(GetAnisoWrapSampler(), pin.TexC);

    clip(diffuseAlbedo.a - 0.1f);
#endif
}



