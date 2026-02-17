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
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentU : TANGENT;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    vout.PosW = mul(float4(vin.PosL, 1.0f), gWorld).xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);

    vout.TexC = vin.TexC;
    vout.TangentU = mul(vin.TangentU, (float3x3)gWorld);

    return vout;
}

float CalcTessFactor(float3 p, out float tessLevel)
{
    float d = distance(p, gEyePosW);

    float s = saturate((d - gMeshMinTessDist) / (gMeshMaxTessDist - gMeshMinTessDist));

    tessLevel = lerp(gMeshMaxTess, gMeshMinTess, s);
     
    return pow(2, tessLevel);
}

float CalcTessFactor(float3 p)
{
    float tessLevel = 0.0f;
    return CalcTessFactor(p, tessLevel);
}

struct PatchTess
{
    float EdgeTess[3]   : SV_TessFactor;
    float InsideTess[1] : SV_InsideTessFactor;

    // Extra data per patch
    float TessLevel : TESS_LEVEL;
};

PatchTess ConstantHS(InputPatch<VertexOut, 3> patch, uint patchID : SV_PrimitiveID)
{
    PatchTess pt;

    //
    // Frustum cull the triangle--if the box containing the triangle 
    // is outside the frustum then no point tessellating it.
    //

    float3 vMin = min( min( float3(patch[0].PosW), float3(patch[1].PosW) ), float3(patch[2].PosW) );
    float3 vMax = max( max( float3(patch[0].PosW), float3(patch[1].PosW) ), float3(patch[2].PosW) );

    float3 boxCenter  = 0.5f*(vMin + vMax);

    // Inflate box a bit to compensate for displacement mapping.
    float3 boxExtents = 0.5f*(vMax - vMin) + float3(1, 1, 1);

    if(AabbOutsideFrustumTest(boxCenter, boxExtents, gWorldFrustumPlanes))
    {
        pt.EdgeTess[0] = 0.0f;
        pt.EdgeTess[1] = 0.0f;
        pt.EdgeTess[2] = 0.0f;

        pt.InsideTess[0] = 0.0f;

        return pt;
    }
    //
    // Do normal tessellation based on distance.
    //
    else
    
    {
        // It is important to do the tess factor calculation based on the
        // edge properties so that edges shared by more than one patch will
        // have the same tessellation factor.  Otherwise, gaps can appear.

        // Compute midpoint on edges, and patch center
        float3 e0 = 0.5f*(patch[1].PosW + patch[2].PosW);
        float3 e1 = 0.5f*(patch[0].PosW + patch[2].PosW);
        float3 e2 = 0.5f*(patch[0].PosW + patch[1].PosW);

        float3  c = (patch[0].PosW + patch[1].PosW + patch[2].PosW) / 3.0f;

        pt.EdgeTess[0] = CalcTessFactor(e0);
        pt.EdgeTess[1] = CalcTessFactor(e1);
        pt.EdgeTess[2] = CalcTessFactor(e2);

        pt.InsideTess[0] = CalcTessFactor(c, pt.TessLevel);

        return pt;
    }
}

struct HullOut
{
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
    float3 TangentU : TANGENT;
};

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ConstantHS")]
[maxtessfactor(64.0f)]
HullOut HS(InputPatch<VertexOut, 3> p,
           uint i : SV_OutputControlPointID,
           uint patchId : SV_PrimitiveID)
{
    HullOut hout;

    // Pass through shader.
    hout.PosW = p[i].PosW;
    hout.NormalW = p[i].NormalW;
    hout.TexC = p[i].TexC;
    hout.TangentU = p[i].TangentU;

    return hout;
}

struct DomainOut
{
    float4 PosH    : SV_POSITION;
    float4 ShadowPosH : POSITION0;
    float4 SsaoPosH   : POSITION1;
    float3 PosW    : POSITION2;
    float3 NormalW : NORMAL;
    float3 TangentW : TANGENT;
    float2 TexC    : TEXCOORD;
};

[domain("tri")]
DomainOut DS(PatchTess patchTess,
             float3 baryCoords : SV_DomainLocation,
             const OutputPatch<HullOut, 3> tri)
{
    DomainOut dout = (DomainOut)0.0f;

    dout.PosW = 
        baryCoords.x * tri[0].PosW + 
        baryCoords.y * tri[1].PosW + 
        baryCoords.z * tri[2].PosW;

    dout.NormalW = 
        baryCoords.x * tri[0].NormalW + 
        baryCoords.y * tri[1].NormalW + 
        baryCoords.z * tri[2].NormalW;

    dout.TexC = 
        baryCoords.x * tri[0].TexC + 
        baryCoords.y * tri[1].TexC + 
        baryCoords.z * tri[2].TexC;

    dout.TangentW = 
        baryCoords.x * tri[0].TangentU + 
        baryCoords.y * tri[1].TangentU + 
        baryCoords.z * tri[2].TangentU;

    dout.NormalW = normalize( dout.NormalW );

    // Reorthogonalize the normal and tangent.
    dout.TangentW = normalize(dout.TangentW - dot( dout.NormalW, dout.TangentW ) * dout.NormalW);

    float4 posW = float4(dout.PosW, 1.0f);

    MaterialData matData = gMaterialData[gMaterialIndex];

    // Output vertex attributes for interpolation across triangle.
    float4 texC = mul(float4(dout.TexC, 0.0f, 1.0f), gTexTransform);
    dout.TexC = mul(texC, matData.MatTransform).xy;

    uint glossHeightAoMapIndex = matData.GlossHeightAoMapIndex;
    Texture2D glossHeightAoMap = ResourceDescriptorHeap[glossHeightAoMapIndex];

    // Do displacement mapping. Use the interior triangle TessLevel
    // as a way to estimate the mipLevel to prevent oversampling artifacts.
    float mipLevel = gMeshMaxTess - patchTess.TessLevel;
    float layerHeight = glossHeightAoMap.SampleLevel(GetAnisoWrapSampler(), dout.TexC, mipLevel).g;
    posW.xyz += matData.DisplacementScale*layerHeight*dout.NormalW;


    // Transform to homogeneous clip space.
    dout.PosH = mul(posW, gViewProj);

    if( gSsaoEnabled )
    {
        // Generate projective tex-coords to project SSAO map onto scene.
        dout.SsaoPosH = mul(posW, gViewProjTex);
    }

    if( gShadowsEnabled )
    {
        // Generate projective tex-coords to project shadow map onto scene.
        dout.ShadowPosH = mul(posW, gShadowTransform);
    }
    
    return dout;
}

