//=============================================================================
// SsaoBlur.hlsl by Frank Luna (C) 2023 All Rights Reserved.
//
// Performs a bilateral edge preserving blur of the ambient map.  We use 
// a pixel shader instead of compute shader to avoid the switch from 
// compute mode to rendering mode.  The texture cache makes up for some of the
// loss of not having shared memory.  The ambient map uses 16-bit texture
// format, which is small, so we should be able to fit a lot of texels
// in the cache.
//=============================================================================

// Include common HLSL code.
#include "Shaders/Common.hlsl"

static const int gSsaoBlurRadius = 5;
 
static const float2 gTexCoords[6] =
{
    float2(0.0f, 1.0f),
    float2(0.0f, 0.0f),
    float2(1.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(1.0f, 1.0f)
};
 
struct VertexOut
{
    float4 PosH  : SV_POSITION;
    float2 TexC  : TEXCOORD;
};

VertexOut VS(uint vid : SV_VertexID)
{
    VertexOut vout;

    vout.TexC = gTexCoords[vid];

    // Quad covering screen in NDC space.
    vout.PosH = float4(2.0f*vout.TexC.x - 1.0f, 1.0f - 2.0f*vout.TexC.y, 0.0f, 1.0f);

    return vout;
}

float NdcDepthToViewDepth(float z_ndc)
{
    // z_ndc = A + B/viewZ, where gProj[2,2]=A and gProj[3,2]=B.
    float viewZ = gProj[3][2] / (z_ndc - gProj[2][2]);
    return viewZ;
}

float4 PS(VertexOut pin) : SV_Target
{
    // unpack into float array.
    float blurWeights[12] =
    {
        gBlurWeights[0].x, gBlurWeights[0].y, gBlurWeights[0].z, gBlurWeights[0].w,
        gBlurWeights[1].x, gBlurWeights[1].y, gBlurWeights[1].z, gBlurWeights[1].w,
        gBlurWeights[2].x, gBlurWeights[2].y, gBlurWeights[2].z, gBlurWeights[2].w,
    };

    uint inputBindlessIndex;
    float2 texOffset;
    if(gHorzBlur)
    {
        texOffset = float2(gInvAmbientMapSize.x, 0.0f);
        inputBindlessIndex = gSsaoAmbientMap0Index;
    }
    else
    {
        texOffset = float2(0.0f, gInvAmbientMapSize.y);
        inputBindlessIndex = gSsaoAmbientMap1Index;
    }

    Texture2D normalMap = ResourceDescriptorHeap[gSceneNormalMapIndex];
    Texture2D depthMap = ResourceDescriptorHeap[gSceneDepthMapIndex];
    Texture2D inputMap = ResourceDescriptorHeap[inputBindlessIndex];

    // The center value always contributes to the sum.
    float4 color      = blurWeights[gSsaoBlurRadius] * inputMap.SampleLevel(GetPointClampSampler(), pin.TexC, 0.0);
    float totalWeight = blurWeights[gSsaoBlurRadius];
     
    float3 centerNormal = normalMap.SampleLevel(GetPointClampSampler(), pin.TexC, 0.0f).xyz;
    float  centerDepth = NdcDepthToViewDepth(
        depthMap.SampleLevel(GetPointClampSampler(), pin.TexC, 0.0f).r);

    for(float i = -gSsaoBlurRadius; i <= gSsaoBlurRadius; ++i)
    {
        // We already added in the center weight.
        if( i == 0 )
            continue;

        float2 tex = pin.TexC + i*texOffset;

        float3 neighborNormal = normalMap.SampleLevel(GetPointClampSampler(), tex, 0.0f).xyz;
        float  neighborDepth  = NdcDepthToViewDepth(
            depthMap.SampleLevel(GetPointClampSampler(), tex, 0.0f).r);

        //
        // If the center value and neighbor values differ too much (either in 
        // normal or depth), then we assume we are sampling across a discontinuity.
        // We discard such samples from the blur.
        //
    
        if( dot(neighborNormal, centerNormal) >= 0.8f &&
            abs(neighborDepth - centerDepth) <= 0.2f )
        {
            float weight = blurWeights[i + gSsaoBlurRadius];

            // Add neighbor pixel to blur.
            color += weight*inputMap.SampleLevel(
                GetPointClampSampler(), tex, 0.0);
        
            totalWeight += weight;
        }
    }

    // Compensate for discarded samples by making total weights sum to 1.
    return color / totalWeight;
}
