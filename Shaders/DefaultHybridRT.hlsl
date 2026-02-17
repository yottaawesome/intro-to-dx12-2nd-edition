//***************************************************************************************
// Default.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

// Include common HLSL code.
#include "Shaders/Common.hlsl"

struct VertexIn
{
	float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
	float2 TexC    : TEXCOORD;
	float3 TangentU : TANGENT;
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
    float4 ShadowPosH : POSITION0;
    float4 ProjTexH   : POSITION1;
    float3 PosW    : POSITION2;
    float3 NormalW : NORMAL;
	float3 TangentW : TANGENT;
	float2 TexC    : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
	VertexOut vout = (VertexOut)0.0f;

	// Fetch the material data.
	MaterialData matData = gMaterialData[gMaterialIndex];
	
    // Transform to world space.
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
	
	vout.TangentW = mul(vin.TangentU, (float3x3)gWorld);

    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);

    // Generate projective tex-coords.
    vout.ProjTexH  = mul(posW, gViewProjTex);

	// Output vertex attributes for interpolation across triangle.
	float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
	vout.TexC = mul(texC, matData.MatTransform).xy;

    if( gShadowsEnabled )
    {
        // Generate projective tex-coords to project shadow map onto scene.
        vout.ShadowPosH = mul(posW, gShadowTransform);
	}

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
	// Fetch the material data.
	MaterialData matData = gMaterialData[gMaterialIndex];
	float4 diffuseAlbedo = matData.DiffuseAlbedo;
	float3 fresnelR0 = matData.FresnelR0;
	float  roughness = matData.Roughness;
	uint diffuseMapIndex = matData.DiffuseMapIndex;
	uint normalMapIndex = matData.NormalMapIndex;
    uint glossHeightAoMapIndex = matData.GlossHeightAoMapIndex;

    // Dynamically look up the texture in the array.
    Texture2D diffuseMap = ResourceDescriptorHeap[diffuseMapIndex];
    diffuseAlbedo *= diffuseMap.Sample(GetAnisoWrapSampler(), pin.TexC);

#ifdef ALPHA_TEST
    // Discard pixel if texture alpha < 0.1.  We do this test as soon 
    // as possible in the shader so that we can potentially exit the
    // shader early, thereby skipping the rest of the shader code.
    clip(diffuseAlbedo.a - 0.1f);
#endif

	// Interpolating normal can unnormalize it, so renormalize it.
    pin.NormalW = normalize(pin.NormalW);
	
    float3 bumpedNormalW = pin.NormalW;
    if( gNormalMapsEnabled )
    {
        Texture2D normalMap = ResourceDescriptorHeap[normalMapIndex];
	    float3 normalMapSample = normalMap.Sample(GetAnisoWrapSampler(), pin.TexC).rgb;
	    bumpedNormalW = NormalSampleToWorldSpace(normalMapSample, pin.NormalW, pin.TangentW);
    }

    Texture2D glossHeightAoMap = ResourceDescriptorHeap[glossHeightAoMapIndex];
    float3 glossHeightAo = glossHeightAoMap.Sample(GetAnisoWrapSampler(), pin.TexC).rgb;

    // Vector from point being lit to eye. 
    float3 toEyeW = normalize(gEyePosW - pin.PosW);

    float ambientAccess = 1.0f;

    // Light terms.
    float4 ambient = ambientAccess*gAmbientLight*diffuseAlbedo;
    ambient *= glossHeightAo.z;

    Texture2D reflectionMap = ResourceDescriptorHeap[gReflectionMapSrvIndex];
    pin.ProjTexH /= pin.ProjTexH.w;
    float4 rayTracedData = reflectionMap.Sample(GetLinearClampSampler(), pin.ProjTexH.xy);


    // Only the first light casts a shadow.
    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    if( gShadowsEnabled )
    {
        // Ray traced shadow result stored in the alpha channel.
        shadowFactor = rayTracedData.a;
    }

    const float shininess = glossHeightAo.x * (1.0f - roughness);
    Material mat = { diffuseAlbedo, fresnelR0, shininess };
    float4 directLight = ComputeLighting(gLights, mat, pin.PosW,
        bumpedNormalW, toEyeW, shadowFactor);

    float4 litColor = ambient + directLight;

	// Add in specular reflections.
    if( gReflectionsEnabled )
    {
        float3 r = reflect(-toEyeW, bumpedNormalW);
        float3 reflectionColor = rayTracedData.rgb;

        float3 fresnelFactor = SchlickFresnel(fresnelR0, bumpedNormalW, r);
        litColor.rgb += ambientAccess*shininess * fresnelFactor * reflectionColor.rgb;
	}

    // Common convention to take alpha from diffuse albedo.
    litColor.a = diffuseAlbedo.a;

    return litColor;
}


