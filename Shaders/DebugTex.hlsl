//***************************************************************************************
// DebugTex.hlsl by Frank Luna (C) 2023 All Rights Reserved.
//***************************************************************************************

// Include common HLSL code.
#include "Shaders/Common.hlsl"

struct VertexIn
{
	float3 PosL    : POSITION;
	float2 TexC    : TEXCOORD;
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
	float2 TexC    : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
	VertexOut vout = (VertexOut)0.0f;

    // Result of this transform is expected to be in homogeneous clip space.
    vout.PosH = mul(float4(vin.PosL, 1.0f), gWorld);
	
	vout.TexC = vin.TexC;
	
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
	Texture2D gDebugMap = ResourceDescriptorHeap[gDebugTexIndex];
    return float4(gDebugMap.Sample(GetLinearWrapSampler(), pin.TexC).rgb, 1.0f);
}


