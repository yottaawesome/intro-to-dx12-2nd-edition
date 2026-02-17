
#include "Shaders/Common.hlsl"

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION0;
    float2 TexC : TEXCOORD0;
};

// We process 64 points per mesh shader group. When expanding to quads,
// we have (64*4)=256 vertices and (64*2)=128 triangles.
#define MAX_POINT_SPRITES 64
#define MAX_MS_VERTS (MAX_POINT_SPRITES * 4)
#define MAX_MS_TRIS  (MAX_POINT_SPRITES * 2)

[outputtopology("triangle")]
[numthreads(MAX_POINT_SPRITES, 1, 1)]
void MS(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 dispatchThreadId : SV_DispatchThreadID,
    out vertices VertexOut outVerts[MAX_MS_VERTS],
    out indices uint3 outIndices[MAX_MS_TRIS])
{
    SetMeshOutputCounts(MAX_MS_VERTS, MAX_MS_TRIS);

    // Parameterize the particle along the helix based on dispatchThreadId.
    const uint globalParticleId = dispatchThreadId.x;
    const float t = globalParticleId / (float)gHelixParticleCount;

    // Generate position along the helix and rotate with time.
    const float x = gHelixRadius * cos(gHelixAngularFrequency * (t + gHelixSpeed*gTotalTime));
    const float z = gHelixRadius * sin(gHelixAngularFrequency * (t + gHelixSpeed*gTotalTime));
    const float y = gHelixHeight * t;

    const float3 centerW = gHelixOrigin + float3(x, y, z);

    //
    // Compute the local coordinate system of the sprite relative to the world space.
    //

    float3 up = float3(0.0f, 1.0f, 0.0f);
    float3 look = gEyePosW - centerW;
    look = normalize(look);
    float3 right = normalize(cross(up, look));
    up = cross(look, right);

    //
    // Expand the particle quad to face the camera.
    //
    float halfWidth = 0.5f*gHelixParticleSize;
    
    float4 v[4];
    v[0] = float4(centerW + halfWidth*right - halfWidth*up, 1.0f);
    v[1] = float4(centerW + halfWidth*right + halfWidth*up, 1.0f);
    v[2] = float4(centerW - halfWidth*right - halfWidth*up, 1.0f);
    v[3] = float4(centerW - halfWidth*right + halfWidth*up, 1.0f);

    float2 texC[4] = 
    {
        float2(0.0f, 1.0f),
        float2(0.0f, 0.0f),
        float2(1.0f, 1.0f),
        float2(1.0f, 0.0f)
    };

    // Vertices/indices are relative to meshlet this group outputs.
    const uint localParticleId = groupThreadId.x;

    for(int i = 0; i < 4; ++i)
    {
        VertexOut vout;
        vout.PosH = mul(v[i], gViewProj);
        vout.PosW = v[i].xyz;
        vout.TexC = texC[i];
        outVerts[localParticleId*4+i] = vout;
    }

    // Index to ith particle vertices.
    const uint3 baseVertexIndex = 4 * localParticleId;

    outIndices[localParticleId*2+0] = baseVertexIndex + uint3(0, 1, 2);
    outIndices[localParticleId*2+1] = baseVertexIndex + uint3(1, 3, 2);
}

float4 PS(VertexOut pin) : SV_Target
{
    Texture2D particleTexture = ResourceDescriptorHeap[gHelixTextureId];
    float4 textureColor = particleTexture.Sample(GetLinearWrapSampler(), pin.TexC);

    return textureColor * gHelixColorTint;
}
