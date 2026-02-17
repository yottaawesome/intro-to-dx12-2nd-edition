
// The box is [-1, 1]^3 in local space.
bool RayAABBIntersection(float3 rayOriginL, float3 rayDirL, out float3 normal, out float3 tangentU, out float2 texC, out float t_hit)
{
    normal = 0.0f;
    tangentU = 0.0f;
    texC = 0.0f;
    t_hit = 0.0f;

    // Simplified version of the Slabs method; see page 572 of Real Time Rendering 2nd Edition. 

    float tmin = -1.0e9f;
    float tmax = +1.0e9f;

    const float3 extents = float3(1.0f, 1.0f, 1.0f);

    // For each slab (a slab is simply two parallel planes, grouped for faster computations).
    for(int i = 0; i < 3; ++i)
    {
        const float e = rayOriginL[i];
        const float f = rayDirL[i];

        const float invF = 1.0f / f;

        if(abs(f) > 1.0e-8f) // ray not parallel to slab planes.
        {
            float tmin_i = (-e + extents[i]) * invF;
            float tmax_i = (-e - extents[i]) * invF;

            if(tmin_i > tmax_i)
            {
                float temp = tmin_i;
                tmin_i = tmax_i;
                tmax_i = temp;
            }

            tmin = max(tmin, tmin_i);
            tmax = min(tmax, tmax_i);

            // Did the ray miss?
            if(tmin > tmax)
                return false;
        }
        else // ray is parallel to slab planes
        {
            // If the ray is parallel and lies between the slabs, we can
            // continue to the next slab, otherwise the ray misses and we are done.

            // If the ray origin lies outside the slab, then the ray misses.
            if(e < -extents[i] || e > extents[i])
                return false;
        }
    }

    t_hit = tmin;

    // Skip intersections behind the ray or intersection farther than a closer intersection we found.
    if(t_hit < RayTMin() || t_hit >= RayTCurrent())
        return false;

    //
    // Figure out normal and texture coordinates.  Basically a cube map look up.
    //

    // Relative to local space of box.
    float3 q = rayOriginL + t_hit * rayDirL;

    int faceIndex = 0;
    texC = CubeLookup(q, faceIndex);

    switch(faceIndex)
    {
    case 0:
        normal = float3(1.0f, 0.0f, 0.0f);
        tangentU = float3(0.0f, 0.0f, 1.0f);
        break;
    case 1:
        normal = float3(-1.0f, 0.0f, 0.0f);
        tangentU = float3(0.0f, 0.0f, -1.0f);
        break;
    case 2:
        normal = float3(0.0f, 1.0f, 0.0f);
        tangentU = float3(1.0f, 0.0f, 0.0f);
        break;
    case 3:
        normal = float3(0.0f, -1.0f, 0.0f);
        tangentU = float3(-1.0f, 0.0f, 0.0f);
        break;
    case 4:
        normal = float3(0.0f, 0.0f, 1.0f);
        tangentU = float3(-1.0f, 0.0f, 0.0f);
        break;
    case 5:
        normal = float3(0.0f, 0.0f, -1.0f);
        tangentU = float3(1.0f, 0.0f, 0.0f);
        break;
    default:
        break;
    }

    return true;
}

// The sphere is centered at origin with radius 1 in local space.
bool RaySpheresIntersection(float3 rayOriginL, float3 rayDirL, out float3 normal, out float3 tangentU, out float2 texC, out float t_hit)
{
    normal = 0.0f;
    tangentU = 0.0f;
    texC = 0.0f;
    t_hit = 0.0f;

    // Solve ||r(t)-c||^2 = r^2
    // 
    // In our local space, c = 0 and r = 1.

    const float radius = 1.0f;
    const float radiusSquared = 1.0f;

    const float3 m = rayOriginL;

    const float a = dot(rayDirL, rayDirL);
    const float b = 2.0f*dot(m, rayDirL);
    const float c = dot(m, m) - radiusSquared;

    const float disc = b*b - 4.0f*a*c;

    if(disc < 0.0f)
        return false;

    const float oneOverTwoA = 1.0f / (2.0f*a);

    const float t1 = (-b + sqrt(disc)) * oneOverTwoA;
    const float t2 = (-b - sqrt(disc)) * oneOverTwoA;

    t_hit = min(t1, t2);

    // Skip intersections behind the ray or intersection farther than a closer intersection we found.
    if(t_hit < RayTMin() || t_hit >= RayTCurrent())
        return false;

    const float3 hitPoint = rayOriginL + t_hit * rayDirL;
    normal = normalize(hitPoint);

    const float theta = atan2(hitPoint.z, hitPoint.x);
    const float phi = acos(hitPoint.y);

    // Partial derivative of P with respect to theta
    const float sinPhi = sin(phi);
    float sinTheta, cosTheta;
    sincos(theta, sinTheta, cosTheta);
	tangentU.x = -radius*sinPhi*sinTheta;
	tangentU.y = 0.0f;
	tangentU.z = +radius*sinPhi*cosTheta;

    tangentU = normalize(tangentU);

    const float Pi = 3.1415926;
    texC.x = theta / (2.0f * Pi);
    texC.y = phi / Pi;

    return true;
}

//
// The disk's is centered at (0, offsetY, 0) with radius 1 and lies on the plane y = offsetY in local space.
// This is a helper function to add caps onto the cylinder in RayCappedCylinderIntersection().
//
bool RayDiskIntersection(float3 rayOriginL, float3 rayDirL, out float3 normal, out float3 tangentU, out float2 texC, out float t_hit)
{
    normal = 0.0f;
    tangentU = 0.0f;
    texC = 0.0f;
    t_hit = 0.0f;

    const float radius = 1.0f;
    const float3 center = float3(0.0f, 0.0f, 0.0f);
    const float3 n = float3(0.0f, 1.0f, 0.0f);
    const float d = -center.y * n.y;

    // Check if the ray is parallel to the plane.
    const float denom = n.y * rayDirL.y;
    if(abs(denom) < 0.00001f)
        return false;

    // Ray/plane intersection.
    t_hit = -(d + n.y * rayOriginL.y) / denom;

    // Skip intersections behind the ray or intersection farther than a closer intersection we found.
    if(t_hit >= RayTMin() && t_hit <= RayTCurrent())
    {
        const float3 hitpoint = rayOriginL + t_hit*rayDirL;
        const float3 q = hitpoint - center;

        normal = n;
        tangentU = float3(1.0f, 0.0f, 0.0f);
        texC = hitpoint.xz;

        // Check if we are in the disk.
        return dot(q, q) <= radius*radius;
    }

    return false;
}

//
// The cylinder is centered at the origin, aligned with +y axis, has radius 1 and length 2 in local space.
//
bool RayCylinderIntersection(float3 rayOriginL, float3 rayDirL, out float3 normal, out float3 tangentU, out float2 texC, out float t_hit)
{
    normal = 0.0f;
    tangentU = 0.0f;
    texC = 0.0f;
    t_hit = 0.0f;

    const float3 anchor = float3(0.0f, -1.0f, 0.0f);
    const float3 axis = float3(0.0f, 1.0f, 0.0f);
    const float radius = 1.0f;
    const float cylLength = 2.0f;

    const float a = rayDirL.x * rayDirL.x + rayDirL.z * rayDirL.z;

    // a == 0 means ray.direction and cylinder.Axis must be parallel (their cross product is zero).
    if(abs(a) < 0.0001f)
        return false;

    // The 2 in the b'=2b can get factored out of the discriminant (sqrt(2^2*b^2 - 4ac) = 2sqrt(b*b - ac),
    // and then the 2 gets canceled with the 2a.
    const float b = rayOriginL.x * rayDirL.x + rayOriginL.z * rayDirL.z;
    const float c = rayOriginL.x*rayOriginL.x + rayOriginL.z*rayOriginL.z - radius*radius;

    const float disc = b*b - a*c;

    if(disc < 0.0f)
        return false;

    const float t1 = (-b + sqrt(disc)) / a;
    const float t2 = (-b - sqrt(disc)) / a;

    t_hit = min(t1, t2);

    // Skip intersections behind the ray or intersection farther than a closer intersection we found.
    if(t_hit < RayTMin() || t_hit >= RayTCurrent())
        return false;

    const float3 hitPoint = rayOriginL + t_hit * rayDirL;

    const float3 u = hitPoint - anchor;

    // Project onto unit cylinder axis to make sure we intersected the finite cylinder.
    const float k = dot(axis, u);
    if(k <= 0.0f || k >= cylLength)
        return false;

    const float3 projU = k*axis;
    const float3 perpU = u - projU;

    normal = normalize(perpU);

    const float theta = atan2(hitPoint.z, hitPoint.x);

    // Parameterize unit circle:
    //   x(t) = cos(t)
	//   y(t) = 0
	//   z(t) = sin(t)
	// 
	//  dx/dt = -sin(t)
	//  dy/dt = 0
	//  dz/dt = +cos(t)
    tangentU.x = -sin(theta);
    tangentU.y = 0.0f;
    tangentU.z = +cos(theta);
    tangentU = normalize(tangentU);

    const float Pi = 3.1415926;
    texC.x = theta / (2.0f * Pi);
    texC.y = (hitPoint.y - 1.0f) / -cylLength;

    return true;
}

