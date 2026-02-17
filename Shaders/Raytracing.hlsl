
#include "Shaders/Common.hlsl"
#include "Shaders/Intersections.hlsl"

struct LocalRootConstants
{
    uint MaterialIndex;
    uint PrimitiveType;
    float2 TexScale;
};

RaytracingAccelerationStructure gSceneTlas : register(t1);
ConstantBuffer<LocalRootConstants> gLocalConstants : register(b0);



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
    rayDesc.TMin = 0.001f;
    rayDesc.TMax = 100000.0f;

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
    rayDesc.TMax = 100000.0f;

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

[shader("raygeneration")]
void RaygenShader()
{
    uint2 rayIndex = DispatchRaysIndex().xy;
    uint2 imageSize = DispatchRaysDimensions().xy;

    float2 screenPos = rayIndex + 0.5f; // offset to pixel center.

    // Remap to NDC space [-1, 1].
    float2 posNdc = (screenPos / imageSize) * 2.0f - 1.0f;
    posNdc.y = -posNdc.y; // +y up

    // Transform NDC to world space.
    float4 posW = mul(float4(posNdc, 0.0f, 1.0f), gInvViewProj);
    posW.xyz /= posW.w;

    float3 rayOriginW = gEyePosW;
    float3 rayDirW = normalize(posW.xyz - rayOriginW);

    uint currentRecursionDepth = 0;
    float4 color = CastColorRay( rayOriginW, rayDirW, currentRecursionDepth );

    // Write the raytraced color to the output texture.

    RWTexture2D<float4> outputImage = ResourceDescriptorHeap[gRayTraceImageIndex];
    outputImage[rayIndex] = color;
}



[shader("closesthit")]
void ClosestHit(inout ColorRayPayload rayPayload, in GeoAttributes attr)
{
    // This is very similar code to our "Default.hlsl" pixel shader that shades models.

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
    float2 texC = attr.TexC * texScale;

    // Dynamically look up the texture in the array.
    Texture2D diffuseMap = ResourceDescriptorHeap[diffuseMapIndex];
    diffuseAlbedo *= diffuseMap.SampleLevel(GetAnisoWrapSampler(), texC, 0.0f);

    if( diffuseAlbedo.a < 0.1f )
    {
        return;
    }

    float3x3 toWorld = (float3x3)ObjectToWorld4x3();
    float3x3 toWorldInvTranspose = transpose(inverse(toWorld));

    float3 normalW = normalize(mul(attr.Normal, toWorldInvTranspose));
    float3 tangentW = mul(attr.TangentU, toWorld);

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

    // Refraction
    if( transparencyScale > 0.0f )
    {
        float3 refractionAmt = transparencyScale * (1.0f - reflectionAmt);
        float3 refractDir = refract(WorldRayDirection(), bumpedNormalW, indexOfRefraction);
        float4 refractColor = CastColorRay(hitPosW, refractDir, rayPayload.RecursionDepth + 1);
        litColor.rgb = lerp( litColor.rgb, refractColor.rgb, refractionAmt);
    }

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

[shader("intersection")]
void PrimitiveIntersectionShader()
{
    float3 rayOriginL = ObjectRayOrigin();
    float3 rayDirL = ObjectRayDirection();

    uint primitiveType =  gLocalConstants.PrimitiveType;

    float3 normal;
    float3 tangentU;
    float2 texC;
    float t_hit;

    bool hitResult = false;
    switch(primitiveType)
    {
    case GEO_TYPE_BOX: 
        hitResult = RayAABBIntersection(rayOriginL, rayDirL, normal, tangentU, texC, t_hit);
        break;
    case GEO_TYPE_SPHERE: 
        hitResult = RaySpheresIntersection(rayOriginL, rayDirL, normal, tangentU, texC, t_hit);
        break;
    case GEO_TYPE_CYLINDER:
        hitResult = RayCylinderIntersection(rayOriginL, rayDirL, normal, tangentU, texC, t_hit);
        break;
    case GEO_TYPE_DISK:
        hitResult = RayDiskIntersection(rayOriginL, rayDirL, normal, tangentU, texC, t_hit);
        break;
    default:
        break;
    }

    GeoAttributes attr = (GeoAttributes)0;
    if (hitResult)
    {
        attr.Normal = normal;
        attr.TangentU = tangentU;
        attr.TexC = texC;

        ReportHit(t_hit, /*hitKind*/ 0, attr);
    }
}
