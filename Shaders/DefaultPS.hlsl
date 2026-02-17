// Include common HLSL code.
#include "Shaders/Common.hlsl"

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

float4 PS(VertexOut pin) : SV_Target
{
    // Fetch the material data.
#if DRAW_INSTANCED
    MaterialData matData = gMaterialData[pin.MatIndex];
    uint cubeMapIndex = pin.CubeMapIndex;
#else
    MaterialData matData = gMaterialData[gMaterialIndex];
    uint cubeMapIndex = gCubeMapIndex;
#endif

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
    float3 glossHeightAo =  glossHeightAoMap.Sample(GetAnisoWrapSampler(), pin.TexC).rgb;

    // Vector from point being lit to eye. 
    float3 toEyeW = normalize(gEyePosW - pin.PosW);

    float ambientAccess = 1.0f;
    if( gSsaoEnabled )
    {
        // Finish texture projection and sample SSAO map.
        pin.SsaoPosH /= pin.SsaoPosH.w;
        Texture2D ssaoMap = ResourceDescriptorHeap[gSsaoAmbientMap0Index];
        ambientAccess = ssaoMap.Sample(GetLinearClampSampler(), pin.SsaoPosH.xy, 0.0f).r;
    }

    // Light terms.
    float4 ambient = ambientAccess*gAmbientLight*diffuseAlbedo;
    ambient *= glossHeightAo.z;

    // Only the first light casts a shadow.
    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    if( gShadowsEnabled )
    {
        shadowFactor[0] = CalcShadowFactor(pin.ShadowPosH);
    }

    const float shininess = glossHeightAo.x * (1.0f - roughness);
    Material mat = { diffuseAlbedo, fresnelR0, shininess };
    float4 directLight = ComputeLighting(gLights, mat, pin.PosW,
        bumpedNormalW, toEyeW, shadowFactor);

    float4 litColor = ambient + directLight;

    // Add in specular reflections.
    if( gReflectionsEnabled )
    {
        TextureCube gCubeMap = ResourceDescriptorHeap[cubeMapIndex];
        float3 r = reflect(-toEyeW, bumpedNormalW);
        float4 reflectionColor = gCubeMap.Sample(GetLinearWrapSampler(), r);
        float3 fresnelFactor = SchlickFresnel(fresnelR0, bumpedNormalW, r);
        litColor.rgb += ambientAccess*shininess * fresnelFactor * reflectionColor.rgb;
    }

    // Common convention to take alpha from diffuse albedo.
    litColor.a = diffuseAlbedo.a;

    return litColor;
}


