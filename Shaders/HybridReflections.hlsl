
#include "Shaders/Common.hlsl"

struct LocalRootConstants
{
    uint MaterialIndex;
    uint VertexBufferBindlessIndex;
    uint VertexBufferOffset;
    uint IndexBufferBindlessIndex;
    uint IndexBufferOffset;
    float2 TexScale;
};

RaytracingAccelerationStructure gSceneTlas : register(t1);
ConstantBuffer<LocalRootConstants> gLocalConstants : register(b0);


#define MAX_DIST 1000000.0f;

bool CastShadowRay(float3 rayOrigin, float3 rayDir, uint recursionDepth)
{
    // At the maximum recursion depth, TraceRay() calls result in the device going into removed state.
    if( recursionDepth >= MAX_RECURSION_DEPTH )
    {
        return false;
    }

    RayDesc rayDesc;
    rayDesc.Origin = rayOrigin;
    rayDesc.Direction = rayDir;
    rayDesc.TMin = 0.05f;
    rayDesc.TMax = MAX_DIST;

    // Custom data we attach to ray.
    ShadowRayPayload payload;

    // Assume hit, miss shader sets to false.
    payload.Hit = true;

    // For shadows we just care if we hit something. So optimize the ray trace.
    const uint shadowRayFlags = 
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | 
        RAY_FLAG_FORCE_OPAQUE | 
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER;

    // Can be used to filter out geometry instances from raytracing. Only the lower 8-bits are used.
    // This value could come from a constant buffer to filter out different groups of instances.
    uint instanceMask = 0xffffffff;

    // Parameters for indexing shader binding table (SBT).
    uint rayOffset = SHADOW_RAY_TYPE;
    uint rayStride = NUM_RAY_TYPES;
    uint missRayOffset = SHADOW_RAY_TYPE;

    TraceRay(gSceneTlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES | shadowRayFlags, 
        instanceMask, rayOffset, rayStride, missRayOffset, rayDesc, payload);

    return payload.Hit;
}

float4 CastColorRay(float3 rayOrigin, float3 rayDir, uint recursionDepth)
{
    // At the maximum recursion depth, TraceRay() calls result in the device going into removed state.
    if( recursionDepth >= MAX_RECURSION_DEPTH )
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    RayDesc rayDesc;
    rayDesc.Origin = rayOrigin;
    rayDesc.Direction = rayDir;
    rayDesc.TMin = 0.001f;
    rayDesc.TMax = MAX_DIST;

    // Custom data we attach to ray.
    ColorRayPayload payload;
    payload.Color = float4(0, 0, 0, 0);
    payload.RecursionDepth = recursionDepth;

    // Can be used to filter out geometry instances from raytracing. Only the lower 8-bits are used.
    // This value could come from a constant buffer to filter out different groups of instances.
    uint instanceMask = 0xffffffff;

    // Parameters for indexing shader binding table (SBT).
    uint rayOffset = COLOR_RAY_TYPE;
    uint rayStride = NUM_RAY_TYPES;
    uint missRayOffset = COLOR_RAY_TYPE;

    TraceRay(gSceneTlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 
        instanceMask, rayOffset, rayStride, missRayOffset, rayDesc, payload);

    return payload.Color;
}

float NdcDepthToViewDepth(float z_ndc)
{
    // z_ndc = A + B/viewZ, where gProj[2,2]=A and gProj[3,2]=B.
    float viewZ = gProj[3][2] / (z_ndc - gProj[2][2]);
    return viewZ;
}

[shader("raygeneration")]
void RaygenShader()
{
    uint2 rayIndex = DispatchRaysIndex().xy;
    uint2 imageSize = DispatchRaysDimensions().xy;

    float2 screenPos = rayIndex + 0.5f; // offset to pixel center.

    // Remap to NDC space [-1, 1].
    float2 posNdc = (screenPos / imageSize) * 2.0f - 1.0f;
    posNdc.y = -posNdc.y; // +y up

    // Transform NDC to view space.
    float4 rayDirV = mul(float4(posNdc, 0.0f, 1.0f), gInvProj);
    rayDirV.xyz /= rayDirV.w;

	Texture2D depthMap = ResourceDescriptorHeap[gSceneDepthMapIndex];
    Texture2D normalMap = ResourceDescriptorHeap[gSceneNormalMapIndex];

    float depthNdc = depthMap.Load(int3(rayIndex.x, rayIndex.y, 0)).r;
    float3 bumpedNormalW = normalMap.Load(int3(rayIndex.x, rayIndex.y, 0)).xyz;

    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float shadowFactor = 1.0f;

    // Filter pixels that never got written.
    if( depthNdc < 1.0f )
    {
        //
        // Reconstruct 3D world space position and normal.
        //
        float depthV = NdcDepthToViewDepth(depthNdc);

        // Scale by t such that t*rayDirV.z = depthV
        float3 posV = (depthV / rayDirV.z) * rayDirV.xyz;

        float3 posW = mul(float4(posV, 1.0f), gInvView).xyz;

        uint currentRecursionDepth = 0;

        //
        // Cast primary shadow ray from reconstructed position.
        //

        // Only the first light casts a shadow and we assume it is a directional light.
        float3 lightVec = -gLights[0].Direction;

        // Instead of sampling shadow map, cast a shadow ray.
        bool shadowRayHit = CastShadowRay(posW, lightVec, currentRecursionDepth);
        shadowFactor = shadowRayHit ? 0.0f : 1.0f;

        //
        // Cast primary reflection ray from reconstructed position.
        //

        float3 rayOriginW = gEyePosW;
        float3 rayDirW = normalize(posW - rayOriginW);
        float3 reflectionDirW = reflect(rayDirW, bumpedNormalW);

        float4 reflectionColor = CastColorRay(posW, reflectionDirW, currentRecursionDepth);
        result.rgb = reflectionColor.rgb;
    }
    
    RWTexture2D<float4> reflectionMap = ResourceDescriptorHeap[gReflectionMapUavIndex];
    reflectionMap[rayIndex] = float4(result.rgb, shadowFactor);
}


[shader("closesthit")]
void ClosestHit(inout ColorRayPayload rayPayload, in BuiltInTriangleIntersectionAttributes attr)
{
    // For simplicity, we use 32-bit indices in this demo. But you can use 
    // a ByteAddressBuffer and pack two 16-bit indices per dword.
    StructuredBuffer<RTVertex> vertexBuffer = ResourceDescriptorHeap[gLocalConstants.VertexBufferBindlessIndex];
	StructuredBuffer<uint> indexBuffer      = ResourceDescriptorHeap[gLocalConstants.IndexBufferBindlessIndex];

    // Get the vertices of the triangle we hit.
    uint triangleID = PrimitiveIndex();

    uint index0 = indexBuffer[gLocalConstants.IndexBufferOffset + triangleID * 3 + 0];
    uint index1 = indexBuffer[gLocalConstants.IndexBufferOffset + triangleID * 3 + 1];
    uint index2 = indexBuffer[gLocalConstants.IndexBufferOffset + triangleID * 3 + 2];

    RTVertex v0 = vertexBuffer[gLocalConstants.VertexBufferOffset + index0];
    RTVertex v1 = vertexBuffer[gLocalConstants.VertexBufferOffset + index1];
    RTVertex v2 = vertexBuffer[gLocalConstants.VertexBufferOffset + index2];

    // Use the barycentric coordinates to interpolate the vertex attributes at the hit point.
    float3 baryCoords = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);

    float3 normalL = baryCoords.x * v0.Normal + baryCoords.y * v1.Normal + baryCoords.z * v2.Normal;
    float3 tangentL = baryCoords.x * v0.TangentU + baryCoords.y * v1.TangentU + baryCoords.z * v2.TangentU;
    float2 texCoords = baryCoords.x * v0.TexC + baryCoords.y * v1.TexC + baryCoords.z * v2.TexC;

    const float t = RayTCurrent();
    float3 hitPosW = WorldRayOrigin() + t * WorldRayDirection();

    // Fetch the material data.
    uint materialIndex = gLocalConstants.MaterialIndex;
	MaterialData matData = gMaterialData[materialIndex];
	float4 diffuseAlbedo = matData.DiffuseAlbedo;
	float3 fresnelR0 = matData.FresnelR0;
	float  roughness = matData.Roughness;
    float transparencyScale = matData.TransparencyWeight;
    float indexOfRefraction = matData.IndexOfRefraction;

	uint diffuseMapIndex = matData.DiffuseMapIndex;
	uint normalMapIndex = matData.NormalMapIndex;
    uint glossHeightAoMapIndex = matData.GlossHeightAoMapIndex;

    float2 texScale = gLocalConstants.TexScale;
    float2 texC = texCoords * texScale;

    // Dynamically look up the texture in the array.
    Texture2D diffuseMap = ResourceDescriptorHeap[diffuseMapIndex];
    diffuseAlbedo *= diffuseMap.SampleLevel(GetAnisoWrapSampler(), texC, 0.0f);

    if( diffuseAlbedo.a < 0.1f )
    {
        return;
    }

    float3x3 toWorld = (float3x3)ObjectToWorld4x3();

    float3 normalW = normalize(mul(normalL, toWorld));
    float3 tangentW = mul(tangentL, toWorld);

    Texture2D normalMap = ResourceDescriptorHeap[normalMapIndex];
	float3 normalMapSample = normalMap.SampleLevel(GetAnisoWrapSampler(), texC, 0.0f).rgb;
	float3 bumpedNormalW = NormalSampleToWorldSpace(normalMapSample, normalW, tangentW);

    Texture2D glossHeightAoMap = ResourceDescriptorHeap[glossHeightAoMapIndex];
    float3 glossHeightAo = glossHeightAoMap.SampleLevel(GetAnisoWrapSampler(), texC, 0.0f).rgb;

	// Uncomment to turn off normal mapping.
    //bumpedNormalW = normalW;

    // Vector from point being lit to eye. 
    float3 toEyeW = normalize(gEyePosW - hitPosW);

    // Light terms.
    float4 ambient = gAmbientLight*diffuseAlbedo;
    ambient *= glossHeightAo.z;

    // Only the first light casts a shadow and we assume it is a directional light.
    float3 lightVec = -gLights[0].Direction;

    // Instead of sampling shadow map, cast a shadow ray.
    bool shadowRayHit = CastShadowRay(hitPosW, lightVec, rayPayload.RecursionDepth + 1);
    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    shadowFactor[0] = shadowRayHit ? 0.0f : 1.0f;

    const float shininess = glossHeightAo.x * (1.0f - roughness);
    Material mat = { diffuseAlbedo, fresnelR0, shininess };
    float4 directLight = ComputeLighting(gLights, mat, hitPosW,
        bumpedNormalW, toEyeW, shadowFactor);

    float4 litColor = ambient + directLight;

    // Instead of sampling cube map, cast a reflection ray. Observe we capture local reflections!
    float3 reflectionDir = reflect(WorldRayDirection(), bumpedNormalW);
    float4 reflectionColor = CastColorRay(hitPosW, reflectionDir, rayPayload.RecursionDepth + 1);
    float3 fresnelFactor = SchlickFresnel(fresnelR0, bumpedNormalW, reflectionDir);
    float3 reflectionAmt = shininess * fresnelFactor;
    litColor.rgb += reflectionAmt * reflectionColor.rgb;

    rayPayload.Color = litColor;
}

[shader("miss")]
void Color_MissShader(inout ColorRayPayload rayPayload)
{
    TextureCube gCubeMap = ResourceDescriptorHeap[gSkyBoxIndex];
    rayPayload.Color = gCubeMap.SampleLevel(GetLinearWrapSampler(), WorldRayDirection(), 0.0f);
}

[shader("miss")]
void Shadow_MissShader(inout ShadowRayPayload rayPayload)
{
    rayPayload.Hit = false;
}

 