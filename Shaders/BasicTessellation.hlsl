// Include common HLSL code.
#include "Shaders/Common.hlsl"


struct VertexIn
{
	float3 PosL    : POSITION;
};

struct VertexOut
{
	float3 PosL    : POSITION;
};

VertexOut VS(VertexIn vin)
{
	VertexOut vout;
	
	vout.PosL = vin.PosL;

	return vout;
}
 
struct PatchTess
{
	float EdgeTess[4]   : SV_TessFactor;
	float InsideTess[2] : SV_InsideTessFactor;
};

PatchTess ConstantHS(InputPatch<VertexOut, 4> patch, uint patchID : SV_PrimitiveID)
{
	PatchTess pt;
	
	float3 centerL = 0.25f*(patch[0].PosL + patch[1].PosL + patch[2].PosL + patch[3].PosL);
	float3 centerW = mul(float4(centerL, 1.0f), gWorld).xyz;
	
	float d = distance(centerW, gEyePosW);

	// Tessellate the patch based on distance from the eye such that
	// the tessellation is 0 if d >= d1 and 64 if d <= d0.  The interval
	// [d0, d1] defines the range we tessellate in.
	
	const float d0 = 10.0f;
	const float d1 = 100.0f;
	float tess = 64.0f*saturate( (d1-d)/(d1-d0) );

	// Uniformly tessellate the patch.

	pt.EdgeTess[0] = tess;
	pt.EdgeTess[1] = tess;
	pt.EdgeTess[2] = tess;
	pt.EdgeTess[3] = tess;
	
	pt.InsideTess[0] = tess;
	pt.InsideTess[1] = tess;
	
	return pt;
}

struct HullOut
{
	float3 PosL : POSITION;
};

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("ConstantHS")]
[maxtessfactor(64.0f)]
HullOut HS(InputPatch<VertexOut, 4> p, 
           uint i : SV_OutputControlPointID,
           uint patchId : SV_PrimitiveID)
{
	HullOut hout;
	
	hout.PosL = p[i].PosL;
	
	return hout;
}

struct DomainOut
{
	float4 PosH : SV_POSITION;
};

// The domain shader is called for every vertex created by the tessellator.  
// It is like the vertex shader after tessellation.
[domain("quad")]
DomainOut DS(PatchTess patchTess, 
             float2 uv : SV_DomainLocation, 
             const OutputPatch<HullOut, 4> quad)
{
	DomainOut dout;
	
	// Bilinear interpolation.
	float3 v1 = lerp(quad[0].PosL, quad[1].PosL, uv.x); 
	float3 v2 = lerp(quad[2].PosL, quad[3].PosL, uv.x); 
	float3 p  = lerp(v1, v2, uv.y); 
	
	// Displacement mapping
	p.y = 0.3f*( p.z*sin(p.x) + p.x*cos(p.z) );
	
	float4 posW = mul(float4(p, 1.0f), gWorld);
	dout.PosH = mul(posW, gViewProj);
	
	return dout;
}

float4 PS(DomainOut pin) : SV_Target
{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
