// Include common HLSL code.
#include "Shaders/Common.hlsl"

struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentU : TANGENT;
#if SKINNED
    float3 BoneWeights : WEIGHTS;
    uint4 BoneIndices  : BONEINDICES;
#endif
};

struct VertexOut
{
    float4 PosH    : SV_POSITION;
    float4 ShadowPosH : POSITION0;
    float4 SsaoPosH   : POSITION1;
    float3 PosW    : POSITION2;
    float3 NormalW : NORMAL;
    float3 TangentW : TANGENT;
    float2 TexC    : TEXCOORD;

#if DRAW_INSTANCED
    // nointerpolation is used so the index is not interpolated 
    // across the triangle.
    nointerpolation uint MatIndex : MATINDEX;
    nointerpolation uint CubeMapIndex : CUBEMAP_INDEX;
#endif
};

VertexOut VS(VertexIn vin
    #if DRAW_INSTANCED
    , uint instanceID : SV_InstanceID
    #endif
    )
{
    VertexOut vout = (VertexOut)0.0f;

#if DRAW_INSTANCED
    // Fetch the instance data.
    InstanceData instData = gInstanceData[instanceID];
    float4x4 world = instData.World;
    float4x4 texTransform = instData.TexTransform;
    uint matIndex = instData.MaterialIndex;

    vout.MatIndex = matIndex;
    vout.CubeMapIndex = instData.CubeMapIndex;
    MaterialData matData = gMaterialData[matIndex];
#else
    MaterialData matData = gMaterialData[gMaterialIndex];
    float4x4 world = gWorld;
    float4x4 texTransform = gTexTransform;
#endif

#if SKINNED
    ApplySkinning( vin.BoneWeights, vin.BoneIndices, vin.PosL, vin.NormalL, vin.TangentU.xyz);
#endif

    // Transform to world space.
    float4 posW = mul(float4(vin.PosL, 1.0f), world);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3)world);
    
    vout.TangentW = mul(vin.TangentU, (float3x3)world);

    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);

    if( gSsaoEnabled )
    {
        // Generate projective tex-coords to project SSAO map onto scene.
        vout.SsaoPosH = mul(posW, gViewProjTex);
    }

    // Output vertex attributes for interpolation across triangle.
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), texTransform);
    vout.TexC = mul(texC, matData.MatTransform).xy;

    if( gShadowsEnabled )
    {
        // Generate projective tex-coords to project shadow map onto scene.
        vout.ShadowPosH = mul(posW, gShadowTransform);
    }

    return vout;
}



