
// Put code here that is shared between HLSL code and C++ code.
//
// We also employ macros so that we can essentially share the
// same code between HLSL and C++ instead of having two versions.

#if HLSL_CODE
    
    #define DEFINE_CBUFFER(Name, Reg) cbuffer Name : register(Reg)

#elif __cplusplus

    #pragma once

    #include <cstdint>
    #include <DirectXMath.h>
    
    #define DEFINE_CBUFFER(Name, Reg) struct Name

    #define uint uint32_t
    #define uint2 DirectX::XMUINT2
    #define uint3 DirectX::XMUINT3
    #define uint4 DirectX::XMUINT4

    #define int2 DirectX::XMINT2
    #define int3 DirectX::XMINT3
    #define int4 DirectX::XMINT4

    #define float2 DirectX::XMFLOAT2
    #define float3 DirectX::XMFLOAT3
    #define float4 DirectX::XMFLOAT4
    #define float4x4 DirectX::XMFLOAT4X4

#endif

// Fixed indices in sampler heap.
#define SAM_POINT_WRAP 0
#define SAM_POINT_CLAMP 1
#define SAM_LINEAR_WRAP 2
#define SAM_LINEAR_CLAMP 3
#define SAM_ANISO_WRAP 4
#define SAM_ANISO_CLAMP 5
#define SAM_SHADOW 6



DEFINE_CBUFFER(PerObjectCB, b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
    uint     gMaterialIndex;

    // Used for cube mapping.
    uint     gCubeMapIndex;
    uint2    PerObjectCB_Pad0;
    
    // Used only for objects with tessellation.
    float gMeshMinTessDist;
    float gMeshMaxTessDist;
    float gMeshMinTess;
    float gMeshMaxTess;

    // Add some generic members so we can reuse the same structure for different objects.
    uint4    gMiscUint4;
    float4   gMiscFloat4;
};

struct Light
{
    float3 Strength;
    float FalloffStart;// point/spot light only
    float3 Direction;  // directional/spot light only
    float FalloffEnd;  // point/spot light only
    float3 Position;   // point/spot light only
    float SpotPower;   // spot light only
};

#define MaxLights 16

DEFINE_CBUFFER(PerPassCB, b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4x4 gShadowTransform;
    float4x4 gViewProjTex;

    float4 gWorldFrustumPlanes[6];

    float3 gEyePosW;
    float PerPassCB_pad0;

    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;

    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;

    float4 gAmbientLight;

    // Used in Chapter 10
    float4 gFogColor;
    float gFogStart;
    float gFogRange;

    float2 PerPassCB_pad1;

    uint gSkyBoxIndex;
    uint gSunShadowMapIndex;
    uint gRandomTexIndex;
    uint gRayTraceImageIndex;

    uint gSceneDepthMapIndex;
    uint gSceneNormalMapIndex;
    uint gSsaoAmbientMap0Index;
    uint gSsaoAmbientMap1Index;

    uint gReflectionMapUavIndex;
    uint gReflectionMapSrvIndex;
    uint gDebugTexIndex;
    uint PerPassCB_pad2;

    // TODO: Could pack all these options into 1 uint and use bitflags.
    uint gNormalMapsEnabled;
    uint gReflectionsEnabled;
    uint gShadowsEnabled;
    uint gSsaoEnabled;


    uint gNumDirLights = 1;
    uint gNumPointLights = 0;
    uint gNumSpotLights = 0;
    uint gFogEnabled = 0;

    // Indices [0, gNumDirLights) are directional lights;
    // indices [gNumDirLights, gNumDirLights+gNumPointLights) are point lights;
    // indices [gNumDirLights+gNumPointLights, gNumDirLights+gNumPointLights+gNumSpotLights)
    // are spot lights for a maximum of MaxLights per object.
    Light gLights[MaxLights];
};

// Structured Buffer
struct MaterialData
{
    float4 DiffuseAlbedo;
    float3 FresnelR0;
    float Roughness;
    float DisplacementScale;

    uint DiffuseMapIndex;
    uint NormalMapIndex;
    uint GlossHeightAoMapIndex;

    // Used in texture mapping.
    float4x4 MatTransform;

    // Used in ray tracing demos only.
    float TransparencyWeight;
    float IndexOfRefraction;
};


#define MaxTerrainLayers 8

// Note: We use several different cbuffer structures bound to the same register, but used for different shaders.
// This is okay. The root signature just defines that it expects a cbuffer bound at a
// specific register, but it does not care about the actual contents of that cbuffer--it is just memory.
// The shader code does care, and you can think of the cbuffer struct specifying how to reinterpret 
// the bytes of the cbuffer memory.

DEFINE_CBUFFER(PerTerrainCB, b0)
{
    float4x4 gTerrainWorld;

    float2 gTerrainWorldCellSpacing;
    float2 gTerrainWorldSize;

    float2 gTerrainHeightMapSize;
    float2 gTerrainTexelSizeUV;

    float gTerrainMinTessDist;
    float gTerrainMaxTessDist;
    float gTerrainMinTess;
    float gTerrainMaxTess;

    uint gBlendMap0SrvIndex;
    uint gBlendMap1SrvIndex;
    uint gHeightMapSrvIndex;
    uint gNumTerrainLayers;

    uint4 gTerrainLayerMaterialIndices[2];

    uint gUseTerrainHeightMap;
    uint gUseMaterialHeightMaps;
    uint2 gTerrain_Pad0;

    //
    // Used for Mesh Shader based Terrains
    //
    uint gNumQuadVertsPerTerrainSide;
    uint gTerrainVerticesSrvIndex;
    uint gTerrainGroupBoundsSrvIndex;
    uint gNumAmplificationGroupsX;

    uint gNumAmplificationGroupsY;
    float gSkirtOffsetY;
    uint2 gTerrain_Pad1;
};


DEFINE_CBUFFER(SkinnedCB, b2)
{
    float4x4 gBoneTransforms[96];
};

DEFINE_CBUFFER(GpuWavesCB, b0)
{
    float gWaveConstant0;
    float gWaveConstant1;
    float gWaveConstant2;
    float gDisturbMag;

    uint2 gDisturbIndex;
    uint2 gGridSize;

    uint gPrevSolIndex;
    uint gCurrSolIndex;
    uint gOutputIndex;
    uint GpuWavesCB_Pad0;
};


DEFINE_CBUFFER(BlurDispatchCB, b0)
{
    float4 gWeightVec[8]; // 8*4=32 floats for max blur radius of 15.

    int gBlurRadius;
    uint gBlurInputIndex;
    uint gBlurOutputIndex;
    uint BlurDispatchCB_Pad0;
};



DEFINE_CBUFFER(ParticleUpdateCB, b2)
{
    float3 gAcceleration;
    float ParticleUpdateCB_Pad0;

    uint gParticleBufferUavIndex;
    uint gFreeIndexBufferUavIndex;
    uint gPrevAliveIndexBufferUavIndex;
    uint gCurrAliveIndexBufferUavIndex;

    uint gFreeCountUavIndex;
    uint gPrevAliveCountUavIndex;
    uint gCurrAliveCountUavIndex;
    uint gIndirectArgsUavIndex;
};

DEFINE_CBUFFER(ParticleDrawCB, b0)
{
    uint gParticleBufferIndex;
    uint gParticleCurrAliveBufferIndex;
    uint2 ParticleDrawCB_Pad0;
};

DEFINE_CBUFFER(ParticleEmitCB, b0)
{
    float3 gEmitBoxMin;
    float gMinLifetime;

    float3 gEmitBoxMax;
    float gMaxLifetime;

    float3 gEmitDirectionMin;
    float gMinInitialSpeed;

    float3 gEmitDirectionMax;
    float gMaxInitialSpeed;

    float4 gEmitColorMin;
    float4 gEmitColorMax;

    float gMinRotation;
    float gMaxRotation;
    float gMinRotationSpeed;
    float gMaxRotationSpeed;

    float2 gMinScale;
    float2 gMaxScale;

    float gDragScale;
    uint gEmitCount;
    uint gBindlessTextureIndex;
    uint ParticleEmitCB_pad0;

    float4 gEmitRandomValues;
};

DEFINE_CBUFFER(SsaoCB, b0)
{
    float4 gOffsetVectors[14];

    // For SsaoBlur.hlsl
    float4 gBlurWeights[3];

    // Coordinates given in view space.
    float gOcclusionRadius;
    float gOcclusionFadeStart;
    float gOcclusionFadeEnd;
    float gSurfaceEpsilon;

    float2 gInvAmbientMapSize;
    uint gHorzBlur;
    uint gSsaoCB_Pad;
};

// For mesh shader
DEFINE_CBUFFER(HelixParticlesCB, b0)
{
    float4 gHelixColorTint;

    float3 gHelixOrigin;
    uint gHelixParticleCount;

    float gHelixRadius;
    float gHelixHeight;
    float gHelixAngularFrequency;
    float gHelixParticleSize;

    float gHelixSpeed;
    uint gHelixTextureId;
    uint2 gHelixParticles_Pad0;
};

// Structured Buffer
struct Particle
{
    float3 Position;
    float3 Velocity;
    float2 Size;
    float4 Color;
    float Lifetime;
    float Age;
    float Rotation;
    float RotationSpeed;
    float DragScale;
    uint BindlessTextureIndex;
};

// Structured Buffer
struct InstanceData
{
    float4x4 World;
    float4x4 TexTransform;
    uint     MaterialIndex;
    uint     CubeMapIndex;
};

//////////////
// Ray Tracing
//////////////

// We just cast shadow and color rays.
#define NUM_RAY_TYPES 2
#define COLOR_RAY_TYPE 0
#define SHADOW_RAY_TYPE 1

// The MAX_RECURSION_DEPTH indicates how much stack space is needed for a ray. Keep this
// as low as possible.
// 
// From the spec: https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html
// Recursion depth must be [0, 31], and at the max recursion depth calling TraceRay()
// results in the device going into removed state.
// 
// Depth 0 is the first ray spawn.
#define MAX_RECURSION_DEPTH 3 // primary---> shadow
                              //       L---> reflect ---> shadow
                              //                    L---> reflect

// PrimitiveType
#define GEO_TYPE_BOX 0
#define GEO_TYPE_SPHERE 1
#define GEO_TYPE_CYLINDER 2
#define GEO_TYPE_DISK 3

struct RTVertex
{
    float3 Pos;
    float3 Normal;
    float2 TexC;
    float3 TangentU;
};

struct GeoAttributes
{
    float3 Normal;
    float3 TangentU;
    float2 TexC;
};

struct ColorRayPayload
{
    float4 Color;
    uint   RecursionDepth;
};

struct ShadowRayPayload
{
    bool Hit;
};
