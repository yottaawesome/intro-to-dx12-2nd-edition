//***************************************************************************************
// MathHelper.cpp by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

#include "MathHelper.h"
#include <float.h>
#include <cmath>

using namespace DirectX;
using namespace DirectX::SimpleMath;

const float MathHelper::Infinity = FLT_MAX;
const float MathHelper::Pi       = 3.1415926535f;

float MathHelper::AngleFromXY(float x, float y)
{
	float theta = 0.0f;
 
	// Quadrant I or IV
	if(x >= 0.0f) 
	{
		// If x = 0, then atanf(y/x) = +pi/2 if y > 0
		//                atanf(y/x) = -pi/2 if y < 0
		theta = atanf(y / x); // in [-pi/2, +pi/2]

		if(theta < 0.0f)
			theta += 2.0f*Pi; // in [0, 2*pi).
	}

	// Quadrant II or III
	else      
		theta = atanf(y/x) + Pi; // in [0, 2*pi).

	return theta;
}

XMVECTOR MathHelper::RandUnitVec3()
{
	XMVECTOR One  = XMVectorSet(1.0f, 1.0f, 1.0f, 1.0f);
	XMVECTOR Zero = XMVectorZero();

	// Keep trying until we get a point on/in the sphere.
	while(true)
	{
		// Generate random point in the cube [-1,1]^3.
		XMVECTOR v = XMVectorSet(MathHelper::RandF(-1.0f, 1.0f), MathHelper::RandF(-1.0f, 1.0f), MathHelper::RandF(-1.0f, 1.0f), 0.0f);

		// Ignore points outside the unit sphere in order to get an even distribution 
		// over the unit sphere.  Otherwise points will clump more on the sphere near 
		// the corners of the cube.

		if( XMVector3Greater( XMVector3LengthSq(v), One) )
			continue;

		return XMVector3Normalize(v);
	}
}

XMVECTOR MathHelper::RandHemisphereUnitVec3(XMVECTOR n)
{
	XMVECTOR One  = XMVectorSet(1.0f, 1.0f, 1.0f, 1.0f);
	XMVECTOR Zero = XMVectorZero();

	// Keep trying until we get a point on/in the hemisphere.
	while(true)
	{
		// Generate random point in the cube [-1,1]^3.
		XMVECTOR v = XMVectorSet(MathHelper::RandF(-1.0f, 1.0f), MathHelper::RandF(-1.0f, 1.0f), MathHelper::RandF(-1.0f, 1.0f), 0.0f);

		// Ignore points outside the unit sphere in order to get an even distribution 
		// over the unit sphere.  Otherwise points will clump more on the sphere near 
		// the corners of the cube.
		
		if( XMVector3Greater( XMVector3LengthSq(v), One) )
			continue;

		// Ignore points in the bottom hemisphere.
		if( XMVector3Less( XMVector3Dot(n, v), Zero ) )
			continue;

		return XMVector3Normalize(v);
	}
}

void MathHelper::CalcPickingRay(const Vector2& screenPos,
								const Vector2& screenSize,
								const Matrix& viewMatrix,
								const Matrix& projMatrix,
								Vector3& worldRayOrigin,
								Vector3& worldRayDirection)
{
	
	Matrix invView = viewMatrix.Invert();

	// Compute picking ray in view space.
	float vx = (+2.0f*screenPos.x / screenSize.x - 1.0f) / projMatrix(0, 0);
	float vy = (-2.0f*screenPos.y / screenSize.y + 1.0f) / projMatrix(1, 1);

	// Ray definition in view space.
	Vector3 rayOrigin = Vector3(0.0f, 0.0f, 0.0f);
	Vector3 rayDir = Vector3(vx, vy, 1.0f);

	worldRayOrigin = Vector3::Transform(rayOrigin, invView);
	worldRayDirection = Vector3::TransformNormal(rayDir, invView);

	// Make the ray direction unit length for the intersection tests.
	worldRayDirection.Normalize();
}

void MathHelper::ExtractFrustumPlanes(const Matrix& M, XMFLOAT4 outPlanes[6])
{
	Plane planes[6];

	//
	// Left
	//
	planes[0].x = M(0, 3) + M(0, 0);
	planes[0].y = M(1, 3) + M(1, 0);
	planes[0].z = M(2, 3) + M(2, 0);
	planes[0].w = M(3, 3) + M(3, 0);

	//
	// Right
	//
	planes[1].x = M(0, 3) - M(0, 0);
	planes[1].y = M(1, 3) - M(1, 0);
	planes[1].z = M(2, 3) - M(2, 0);
	planes[1].w = M(3, 3) - M(3, 0);

	//
	// Bottom
	//
	planes[2].x = M(0, 3) + M(0, 1);
	planes[2].y = M(1, 3) + M(1, 1);
	planes[2].z = M(2, 3) + M(2, 1);
	planes[2].w = M(3, 3) + M(3, 1);

	//
	// Top
	//
	planes[3].x = M(0, 3) - M(0, 1);
	planes[3].y = M(1, 3) - M(1, 1);
	planes[3].z = M(2, 3) - M(2, 1);
	planes[3].w = M(3, 3) - M(3, 1);

	//
	// Near
	//
	planes[4].x = M(0, 2);
	planes[4].y = M(1, 2);
	planes[4].z = M(2, 2);
	planes[4].w = M(3, 2);

	//
	// Far
	//
	planes[5].x = M(0, 3) - M(0, 2);
	planes[5].y = M(1, 3) - M(1, 2);
	planes[5].z = M(2, 3) - M(2, 2);
	planes[5].w = M(3, 3) - M(3, 2);

	// Normalize the plane equations.
	for(int i = 0; i < 6; ++i)
	{
		planes[i].Normalize();
		outPlanes[i] = planes[i];
	}
}

BoundingSphere MathHelper::ComputeFrustumBoundingSphereInViewSpace(const BoundingFrustum& subfrustum)
{
	//
	// In view space, the bounding sphere has center C = (0, 0, s) for some s.
	//
	// Let P be a corner point on the near window and let Q be a corner point on the far window.
	//
	// For the circumscribed sphere we have distance(C, P) == distance(C, Q).
	//
	// ||P - C||^2 == ||Q - C||^2
	//
	// dot(P-C,P-C) = dot(Q-C,Q-C)
	//
	// dot(P,P) - 2*dot(P,C) + dot(C,C) == dot(Q,Q) - 2*dot(Q,C) + dot(C,C)
	//
	// -2*dot(P,C) + 2*dot(Q,C) == dot(Q,Q) - dot(P,P)
	//
	// 2 * dot(C, Q-P) == dot(Q,Q) - dot(P,P)
	//
	// Since C = (0, 0, s), dot(C, Q-P) = s(qz - pz)
	//
	// s = (dot(Q,Q) - dot(P,P)) / (2(qz - pz))
	// 
	// The circumscribed sphere is not necessarily the smallest bounding sphere. 
	//
	// Let n be the near plane and let f be the far plane.
	//
	// If n <= s <= f, then the circumscribed sphere is the smallest bounding sphere.
	//
	// If s > f, then C = (0, 0, f) with r = distance(C,Q) gives the smallest bounding sphere.
	// 

	XMFLOAT3 corners[8];
	subfrustum.GetCorners(corners);

	// Point on near plane (left-bottom).
	const Vector3 P = corners[3];

	// Point on far plane (top-right).
	const Vector3 Q = corners[5];

	float n = subfrustum.Near;
	float f = subfrustum.Far;

	float s = (Q.Dot(Q) - P.Dot(P)) / (2.0f*(f - n));

	BoundingSphere result;

	// This only happens if the frustum slope is steep.
	if(s > f)
	{
		result.Center = XMFLOAT3(0.0f, 0.0f, f);
	}
	else
	{
		result.Center  = XMFLOAT3(0.0f, 0.0f, s);
	}

	// This works if s > f, too.
	// r = ||Q - C||
	result.Radius = Vector3::Distance(Q, result.Center);

	return result;
}