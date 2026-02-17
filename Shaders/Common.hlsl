//***************************************************************************************
// Common.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

// Must define HLSL_CODE 1 before #include "SharedTypess.h" in HLSL code.
#define HLSL_CODE 1
#include "SharedTypes.h"

// Include structures and functions for lighting.
#include "LightingUtil.hlsl"

StructuredBuffer<MaterialData> gMaterialData : register(t0);
StructuredBuffer<InstanceData> gInstanceData : register(t1);

SamplerState GetPointWrapSampler()
{
    return SamplerDescriptorHeap[SAM_POINT_WRAP];
}

SamplerState GetPointClampSampler()
{
    return SamplerDescriptorHeap[SAM_POINT_CLAMP];
}

SamplerState GetLinearWrapSampler()
{
    return SamplerDescriptorHeap[SAM_LINEAR_WRAP];
}

SamplerState GetLinearClampSampler()
{
    return SamplerDescriptorHeap[SAM_LINEAR_CLAMP];
}

SamplerState GetAnisoWrapSampler()
{
    return SamplerDescriptorHeap[SAM_ANISO_WRAP];
}

SamplerState GetAnisoClampSampler()
{
    return SamplerDescriptorHeap[SAM_ANISO_CLAMP];
}

SamplerComparisonState GetShadowSampler()
{
    return SamplerDescriptorHeap[SAM_SHADOW];
}


//---------------------------------------------------------------------------------------
// Transforms a normal map sample to world space.
//---------------------------------------------------------------------------------------
float3 NormalSampleToWorldSpace(float3 normalMapSample, float3 unitNormalW, float3 tangentW)
{
	// Uncompress each component from [0,1] to [-1,1].
	float3 normalT = 2.0f*normalMapSample - 1.0f;

	// Build orthonormal basis.
	float3 N = unitNormalW;
	float3 T = normalize(tangentW - dot(tangentW, N)*N);
	float3 B = cross(N, T);

	float3x3 TBN = float3x3(T, B, N);

	// Transform from tangent space to world space.
	float3 bumpedNormalW = mul(normalT, TBN);

	return bumpedNormalW;
}

//---------------------------------------------------------------------------------------
// PCF for shadow mapping.
//---------------------------------------------------------------------------------------

float CalcShadowFactor(float4 shadowPosH)
{
    Texture2D gShadowMap = ResourceDescriptorHeap[gSunShadowMapIndex];

    // Complete projection by doing division by w.
    shadowPosH.xyz /= shadowPosH.w;

    // Depth in NDC space.
    float depth = shadowPosH.z;

    uint width, height, numMips;
    gShadowMap.GetDimensions(0, width, height, numMips);

    // Texel size.
    float dx = 1.0f / (float)width;

    float percentLit = 0.0f;
    const float2 offsets[9] =
    {
        float2(-dx,  -dx), float2(0.0f,  -dx), float2(dx,  -dx),
        float2(-dx, 0.0f), float2(0.0f, 0.0f), float2(dx, 0.0f),
        float2(-dx,  +dx), float2(0.0f,  +dx), float2(dx,  +dx)
    };

    [unroll]
    for(int i = 0; i < 9; ++i)
    {
        percentLit += gShadowMap.SampleCmpLevelZero(GetShadowSampler(),
            shadowPosH.xy + offsets[i], depth).r;
    }
    
    return percentLit / 9.0f;
}

//---------------------------------------------------------------------------------------
// Skinning vertex animation.
//---------------------------------------------------------------------------------------

void ApplySkinning( float3 boneWeights, uint4 boneIndices, inout float3 posL, inout float3 normalL, inout float3 tangentU )
{
    float weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    weights[0] = boneWeights.x;
    weights[1] = boneWeights.y;
    weights[2] = boneWeights.z;
    weights[3] = 1.0f - weights[0] - weights[1] - weights[2];

    float3 skinnedPosL = float3(0.0f, 0.0f, 0.0f);
    float3 skinnedNormalL = float3(0.0f, 0.0f, 0.0f);
    float3 skinnedTangentL = float3(0.0f, 0.0f, 0.0f);
    for(int i = 0; i < 4; ++i)
    {
        // Assume no nonuniform scaling when transforming normals, so 
        // that we do not have to use the inverse-transpose.

        skinnedPosL += weights[i] * mul(float4(posL, 1.0f), gBoneTransforms[boneIndices[i]]).xyz;
        skinnedNormalL += weights[i] * mul(normalL, (float3x3)gBoneTransforms[boneIndices[i]]);
        skinnedTangentL += weights[i] * mul(tangentU.xyz, (float3x3)gBoneTransforms[boneIndices[i]]);
    }

    posL = skinnedPosL;
    normalL = skinnedNormalL;
    tangentU = skinnedTangentL;
}

// For shadows, we only need to animate position.
void ApplySkinningShadows( float3 boneWeights, uint4 boneIndices, inout float3 posL )
{
    float weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    weights[0] = boneWeights.x;
    weights[1] = boneWeights.y;
    weights[2] = boneWeights.z;
    weights[3] = 1.0f - weights[0] - weights[1] - weights[2];

    float3 skinnedPosL = float3(0.0f, 0.0f, 0.0f);
    for(int i = 0; i < 4; ++i)
    {
        // Assume no nonuniform scaling when transforming normals, so 
        // that we do not have to use the inverse-transpose.

        skinnedPosL += weights[i] * mul(float4(posL, 1.0f), gBoneTransforms[boneIndices[i]]).xyz;
    }

    posL = skinnedPosL;
}

//---------------------------------------------------------------------------------------
// Outputs the faceIndex and face 2D texture coordinates given direction vector.
//---------------------------------------------------------------------------------------

float2 CubeLookup(float3 v, out int faceIndex)
{
    float3 vAbs = abs(v);

    float ma;
    float2 uv;
    if(vAbs.z >= vAbs.x && vAbs.z >= vAbs.y)
    {
        faceIndex = v.z < 0.0f ? 5 : 4;
        ma = 0.5f / vAbs.z;
        uv = float2(v.z < 0.0f ? -v.x : v.x, -v.y);
    }
    else if(vAbs.y >= vAbs.x)
    {
        faceIndex = v.y < 0.0f ? 3 : 2;
        ma = 0.5f / vAbs.y;
        uv = float2(v.x, v.y < 0.0f ? -v.z : v.z);
    }
    else
    {
        faceIndex = v.x < 0.0f ? 1 : 0;
        ma = 0.5f / vAbs.x;
        uv = float2(v.x < 0.0f ? v.z : -v.z, -v.y);
    }

    return uv * ma + 0.5f;
}

//---------------------------------------------------------------------------------------
// Returns true if the box is completely behind (in negative half space) of plane.
//---------------------------------------------------------------------------------------
bool AabbBehindPlaneTest(float3 center, float3 extents, float4 plane)
{
	float3 n = abs(plane.xyz);

	// This is always positive.
	float r = dot(extents, n);

	// signed distance from center point to plane.
	float s = dot(float4(center, 1.0f), plane);

	// If the center point of the box is a distance of e or more behind the
	// plane (in which case s is negative since it is behind the plane),
	// then the box is completely in the negative half space of the plane.
	return (s + r) < 0.0f;
}

//---------------------------------------------------------------------------------------
// Returns true if the box is completely outside the frustum.
//---------------------------------------------------------------------------------------
bool AabbOutsideFrustumTest(float3 center, float3 extents, float4 frustumPlanes[6])
{
	for(int i = 0; i < 6; ++i)
	{
		// If the box is completely behind any of the frustum planes
		// then it is outside the frustum.
		if(AabbBehindPlaneTest(center, extents, frustumPlanes[i]))
		{
			return true;
		}
	}

	return false;
}

//---------------------------------------------------------------------------------------
// Matrix-Inverse for 3x3 matrices. We use this to transform normals in the
// procedural ray-tracing demo since we need non-uniform scaling in that demo.
// In practice, it would be better to precompute the inverse-transpose and pass
// through a buffer.
//---------------------------------------------------------------------------------------

float det(const float3x3 M)
{
    float d0 = M[1][1]*M[2][2] - M[1][2]*M[2][1];
    float d1 = M[1][0]*M[2][2] - M[1][2]*M[2][0];
    float d2 = M[1][0]*M[2][1] - M[1][1]*M[2][0];

    return M[0][0] * d0 - M[0][1] * d1 + M[0][2] * d2;
}

float3x3 inverse(const float3x3 M)
{
    // Compute the determinants of all the minors

    float d00 = M[1][1]*M[2][2] - M[1][2]*M[2][1];
    float d01 = M[1][0]*M[2][2] - M[1][2]*M[2][0];
    float d02 = M[1][0]*M[2][1] - M[1][1]*M[2][0];

    float d10 = M[0][1]*M[2][2] - M[0][2]*M[2][1];
    float d11 = M[0][0]*M[2][2] - M[0][2]*M[2][0];
    float d12 = M[0][0]*M[2][1] - M[0][1]*M[2][0];

    float d20 = M[0][1]*M[1][2] - M[0][2]*M[1][1];
    float d21 = M[0][0]*M[1][2] - M[0][2]*M[1][0];
    float d22 = M[0][0]*M[1][1] - M[0][1]*M[1][0];

    // Adjoint is transpose of cofactor matrix
    float3x3 Adj;
    Adj[0][0] = +d00;
    Adj[1][0] = -d01;
    Adj[2][0] = +d02;

    Adj[0][1] = -d10;
    Adj[1][1] = +d11;
    Adj[2][1] = -d12;

    Adj[0][2] = +d20;
    Adj[1][2] = -d21;
    Adj[2][2] = +d22;

    float d = M[0][0] * d00 - M[0][1] * d01 + M[0][2] * d02; 

    return mul(Adj, 1.0f / d);
}