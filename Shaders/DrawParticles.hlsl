
#include "Shaders/Common.hlsl"

struct VertexOut
{
	float4 PosH   : SV_POSITION;
	float4 Color  : COLOR;
	float2 TexC   : TEXCOORD;
	float  Fade   : FADE;
	nointerpolation uint TexIndex : INDEX;
};

VertexOut VS(uint vertexId : SV_VertexID)
{
	RWStructuredBuffer<uint> currAliveIndexBuffer = ResourceDescriptorHeap[gParticleCurrAliveBufferIndex];
	RWStructuredBuffer<Particle> particleBuffer = ResourceDescriptorHeap[gParticleBufferIndex];

	VertexOut vout = (VertexOut)0.0f;

	// Every 4 vertices are used to draw one particle quad.
	uint drawQuadIndex = vertexId / 4;
	uint vertIndex = vertexId % 4;

	uint particleIndex = currAliveIndexBuffer[drawQuadIndex];
	Particle particle = particleBuffer[particleIndex];

	float normalizedLifetime = particle.Age / particle.Lifetime;

	// https://www.slideshare.net/DevCentralAMD/vertex-shader-tricks-bill-bilodeau
	// 
	// 0*----*1     -vertIndex 0 and 2 are even so will have x = -0.5 
	//  |    |      -in binary only vertIndex (2 = 10b) and (3 = 11b) 
	//  |    |       have the 2nd bit set, and hence y = +0.5
	// 2*----*3
	float3 quadVert;
	quadVert.x = (vertIndex % 2) == 0 ? -0.5f : +0.5f;
	quadVert.y = (vertIndex & 2) == 0 ? +0.5f : -0.5f;
	quadVert.z = 0.0f;

	// Remap [-0.5f, 0.5f] --> [0,1], and flip y-axis for tex-coords.
	vout.TexC.x = quadVert.x + 0.5f; 
	vout.TexC.y = 1.0f - (quadVert.y + 0.5f);

	// Rotate in 2d.
	float sinRotation;
	float cosRotation;
	sincos(particle.Rotation, sinRotation, cosRotation);
	float2x2 rotation2d = float2x2(cosRotation, sinRotation, -sinRotation, cosRotation);
	float2 rotatedQuadVert = mul(quadVert.xy, rotation2d);

	// Fade particle size in starting at 75% of initial size.
	float2 size = particle.Size * (0.75f + 0.25f * normalizedLifetime);

	// Fade particles in and out with: f(t) = 4*t*(1-t).  Then f(0) = f(1) = 0, and the maximum is given at f(0.5) = 1.0.
	vout.Fade = 4.0f * normalizedLifetime * (1 - normalizedLifetime);

	//
	// Transform to world so that billboard faces the camera.
	//

	float3 look  = normalize(gEyePosW.xyz - particle.Position);
	float3 right = normalize(cross(float3(0,1,0), look));
	float3 up    = cross(look, right);	

	float2 posL = size * rotatedQuadVert;
	float3 posW = particle.Position - posL.x * right + posL.y * up;

	vout.PosH = mul(float4(posW, 1.0f), gViewProj);

	vout.TexIndex = particle.BindlessTextureIndex;
	vout.Color = particle.Color;

	return vout;
}

float4 PSAddBlend(VertexOut pin) : SV_Target
{
	Texture2D texMap = ResourceDescriptorHeap[pin.TexIndex];

	float4 color = texMap.Sample(GetLinearWrapSampler(), pin.TexC);

	color *= pin.Color;

	color.rgb *= color.a*pin.Fade;

	return color;
}

float4 PSTransparencyBlend(VertexOut pin) : SV_Target
{
	Texture2D texMap = ResourceDescriptorHeap[pin.TexIndex];

	float4 color = texMap.Sample(GetLinearWrapSampler(), pin.TexC);

	color *= pin.Color;

	color.a *= pin.Fade;

	return color;
}