//***************************************************************************************
// MeshGen.h by Frank Luna (C) 2022 All Rights Reserved.
//   
// Defines a static class for procedurally generating the geometry of 
// common mathematical objects.
//
// All triangles are generated "outward" facing.  If you want "inward" 
// facing triangles (for example, if you want to place the camera inside
// a sphere to simulate a sky), you will need to:
//   1. Change the Direct3D cull mode or manually reverse the winding order.
//   2. Invert the normal.
//   3. Update the texture coordinates and tangent vectors.
//***************************************************************************************

#pragma once

#include "MeshUtil.h"
#include <cstdint>
#include <vector>

struct MeshGenVertex
{
    MeshGenVertex() :
        Position(0.0f, 0.0f, 0.0f),
        Normal(0.0f, 0.0f, 0.0f),
        TangentU(0.0f, 0.0f, 0.0f),
        TexC(0.0f, 0.0f) {}
    MeshGenVertex(
        const DirectX::XMFLOAT3& p,
        const DirectX::XMFLOAT3& n,
        const DirectX::XMFLOAT3& t,
        const DirectX::XMFLOAT2& uv) :
        Position(p),
        Normal(n),
        TangentU(t),
        TexC(uv) {}
    MeshGenVertex(
        float px, float py, float pz,
        float nx, float ny, float nz,
        float tx, float ty, float tz,
        float u, float v) :
        Position(px, py, pz),
        Normal(nx, ny, nz),
        TangentU(tx, ty, tz),
        TexC(u, v) {}

    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT3 Normal;
    DirectX::XMFLOAT3 TangentU;
    DirectX::XMFLOAT2 TexC;
};

struct MeshGenData
{
    std::vector<MeshGenVertex> Vertices;
    std::vector<uint32_t> Indices32;

    SubmeshGeometry AppendSubmesh(const MeshGenData& meshData);

    std::vector<uint16_t>& GetIndices16()
    {
        if(mIndices16.empty())
        {
            mIndices16.resize(Indices32.size());
            for(size_t i = 0; i < Indices32.size(); ++i)
                mIndices16[i] = static_cast<uint16_t>(Indices32[i]);
        }

        return mIndices16;
    }

private:
    std::vector<uint16_t> mIndices16;
};

class MeshGen
{
public:

    ///<summary>
    /// Creates a box centered at the origin with the given dimensions, where each
    /// face has m rows and n columns of vertices.
    ///</summary>
    MeshGenData CreateBox(float width, float height, float depth, uint32_t numSubdivisions);

    ///<summary>
    /// Creates a sphere centered at the origin with the given radius.  The
    /// slices and stacks parameters control the degree of tessellation.
    ///</summary>
    MeshGenData CreateSphere(float radius, uint32_t sliceCount, uint32_t stackCount);

    ///<summary>
    /// Creates a geosphere centered at the origin with the given radius.  The
    /// depth controls the level of tessellation.
    ///</summary>
    MeshGenData CreateGeosphere(float radius, uint32_t numSubdivisions);

    ///<summary>
    /// Creates a cylinder parallel to the y-axis, and centered about the origin.  
    /// The bottom and top radius can vary to form various cone shapes rather than true
    // cylinders.  The slices and stacks parameters control the degree of tessellation.
    ///</summary>
    MeshGenData CreateCylinder(float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount);

    ///<summary>
    /// Creates an mxn grid in the xz-plane with m rows and n columns, centered
    /// at the origin with the specified width and depth.
    ///</summary>
    MeshGenData CreateGrid(float width, float depth, uint32_t m, uint32_t n);

    ///<summary>
    /// Creates a quad aligned with the screen.  This is useful for postprocessing and screen effects.
    ///</summary>
    MeshGenData CreateQuad(float x, float y, float w, float h, float depth);

private:

    void Subdivide(MeshGenData& meshData);
    MeshGenVertex MidPoint(const MeshGenVertex& v0, const MeshGenVertex& v1);
    void BuildCylinderTopCap(float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount, MeshGenData& meshData);
    void BuildCylinderBottomCap(float bottomRadius, float topRadius, float height, uint32_t sliceCount, uint32_t stackCount, MeshGenData& meshData);
};

