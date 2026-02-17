// Include common HLSL code.
#include "Shaders/Common.hlsl"

#ifndef WAVES_VS
#define WAVES_VS 0
#endif

struct VertexIn
{
	float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
	float2 TexC    : TEXCOORD;
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION0;
    float3 NormalW : NORMAL;
	float2 TexC    : TEXCOORD;
};

VertexOut VS(VertexIn vin, uint vertexId : SV_VertexID)
{
	VertexOut vout = (VertexOut)0.0f;

	MaterialData matData = gMaterialData[gMaterialIndex];

    // For chapter 13 GPU Waves.
#if WAVES_VS

    uint wavesBufferIndex = gMiscUint4.x;
    uint wavesGridWidth = gMiscUint4.y;
    uint wavesGridDepth = gMiscUint4.z;
    float wavesGridSpatialStep = gMiscFloat4.x;

    Texture2D<float> currSolInput = ResourceDescriptorHeap[wavesBufferIndex];

    uint gridY = vertexId / wavesGridWidth;
    uint gridX = vertexId % wavesGridWidth;

    float waveHeight = currSolInput[uint2(gridX,gridY)].r;
	vin.PosL.y += waveHeight;

	// Estimate normal using finite difference.
    float l = currSolInput[uint2( clamp(gridX-1, 0, wavesGridWidth), gridY)].r;
    float r = currSolInput[uint2( clamp(gridX+1, 0, wavesGridWidth), gridY)].r;
    float t = currSolInput[uint2( gridX, clamp(gridY-1, 0, wavesGridDepth))].r;
    float b = currSolInput[uint2( gridX, clamp(gridY+1, 0, wavesGridDepth))].r;
	vin.NormalL = normalize( float3(-r+l, 2.0f*wavesGridSpatialStep, b-t) );
#endif

    // Transform to world space.
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use 
    // inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
	
    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);

	// Output vertex attributes for interpolation across triangle.
	float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
	vout.TexC = mul(texC, matData.MatTransform).xy;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
	MaterialData matData = gMaterialData[gMaterialIndex];

	float4 diffuseAlbedo = matData.DiffuseAlbedo;
	float3 fresnelR0 = matData.FresnelR0;
	float  roughness = matData.Roughness;
	uint diffuseMapIndex = matData.DiffuseMapIndex;

    // Dynamically look up the texture in the heap.
    Texture2D diffuseMap = ResourceDescriptorHeap[diffuseMapIndex];
    diffuseAlbedo *= diffuseMap.Sample(GetAnisoWrapSampler(), pin.TexC);

#ifdef ALPHA_TEST
    // Discard pixel if texture alpha < 0.1.  We do this test as soon 
    // as possible in the shader so that we can potentially exit the
    // shader early, thereby skipping the rest of the shader code.
    clip(diffuseAlbedo.a - 0.1f);
#endif

	// Interpolating normal can unnormalize it, so renormalize it.
    float3 normalW = normalize(pin.NormalW);
	
    // Vector from point being lit to eye. 
    float3 toEyeW = gEyePosW - pin.PosW;
    float distToEye = length(toEyeW);
    toEyeW /= distToEye; // normalize

    // Light terms.
    float4 ambient = gAmbientLight*diffuseAlbedo;

    const float shininess = (1.0f - roughness);
    Material mat = { diffuseAlbedo, fresnelR0, shininess };
    float4 directLight = ComputeLighting(gLights, mat, pin.PosW,
        normalW, toEyeW);

    float4 litColor = ambient + directLight;

    if( gFogEnabled )
    {
        float fogAmount = saturate((distToEye - gFogStart) / gFogRange);
        litColor = lerp(litColor, gFogColor, fogAmount);
    }

    // Common convention to take alpha from diffuse albedo.
    litColor.a = diffuseAlbedo.a;

    return litColor;
}


