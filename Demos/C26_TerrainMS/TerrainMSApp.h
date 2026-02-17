#pragma once

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/MeshGen.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Common/Camera.h"
#include "../../Common/ShaderLib.h"
#include "../../Common/TextureLib.h"
#include "../../Common/MaterialLib.h"
#include "../../Common/PsoLib.h"
#include "FrameResource.h"
#include "ShadowMap.h"
#include "ParticleSystem.h"
#include "TerrainMS.h"

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
    RenderItem() = default;
    RenderItem(const RenderItem& rhs) = delete;

    // World matrix of the shape that describes the object's local space
    // relative to the world space, which defines the position, orientation,
    // and scale of the object in the world.
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();

    DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

    PerObjectCB ObjectConstants;

    // Handle to memory in linear allocator.
    DirectX::GraphicsResource MemHandleToObjectCB;

    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;

    // Primitive topology.
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

enum class RenderLayer : int
{
    Opaque = 0,
    Debug,
    Sky,
    Count
};


class TerrainMSApp : public D3DApp
{
public:
    TerrainMSApp(HINSTANCE hInstance);
    TerrainMSApp(const TerrainMSApp& rhs) = delete;
    TerrainMSApp& operator=(const TerrainMSApp& rhs) = delete;
    ~TerrainMSApp();

    virtual bool Initialize()override;

private:
    virtual void CreateRtvAndDsvDescriptorHeaps()override;
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void UpdateImgui(const GameTimer& gt)override;
    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void OnKeyboardInput(const GameTimer& gt);
    void AnimateMaterials(const GameTimer& gt);
    void UpdatePerObjectCB(const GameTimer& gt);
    void UpdateMaterialBuffer(const GameTimer& gt);
    void UpdateShadowTransform(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateShadowPassCB(const GameTimer& gt);
    void EmitExplosionParticles(const GameTimer& gt);
    void EmitRainParticles(const GameTimer& gt);
    void ReadParticleCounts(const GameTimer& gt);

    void LoadTextures(); 
    void LoadGeometry();
    void BuildRootSignatures();
    void BuildCommandSignatures();
    void BuildCbvSrvUavDescriptorHeap();
    void BuildShaders();
    void BuildPSOs();
    void BuildFrameResources();

    void BuildMaterials();

    void AddRenderItem(RenderLayer layer, const DirectX::XMFLOAT4X4& world, const DirectX::XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs);
    void BuildRenderItems();

    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
    void DrawSceneToShadowMap();

private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mGfxRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSignature = nullptr;

    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mIndirectDispatch = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mIndirectDrawIndexed = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    uint32_t mRandomTexBindlessIndex = -1;
    uint32_t mSkyBindlessIndex = -1;
    uint32_t mShadowMapBindlessIndex = -1;

    PerPassCB mMainPassCB;  // index 0 of pass cbuffer.
    PerPassCB mShadowPassCB;// index 1 of pass cbuffer.

    Camera mCamera;

    std::unique_ptr<ShadowMap> mShadowMap;

    std::unique_ptr<ParticleSystem> mExplosionParticleSystem;
    std::unique_ptr<ParticleSystem> mRainParticleSystem;

    std::unique_ptr<TerrainMS> mTerrain;

    static constexpr uint32_t MaxExplosionParticleCount = 1024 * 20;
    static constexpr uint32_t MaxRainParticleCount = 1024 * 50;

    DirectX::BoundingSphere mSceneBounds;

    float mLightNearZ = 0.0f;
    float mLightFarZ = 0.0f;
    DirectX::XMFLOAT3 mLightPosW;
    DirectX::XMFLOAT4X4 mLightView = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 mLightProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 mShadowTransform = MathHelper::Identity4x4();

    float mLightRotationAngle = 0.0f;
    DirectX::XMFLOAT3 mBaseLightDirections[3] = {
        DirectX::XMFLOAT3(0.4f, -0.2f, 0.4f),
        DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    DirectX::XMFLOAT3 mRotatedLightDirections[3];

    bool mSpawnExplosion = false;
    DirectX::SimpleMath::Vector3 mWorldRayPos;
    DirectX::SimpleMath::Vector3 mWorldRayDir;

    uint32_t mRainParticleCount = 0;
    uint32_t mDisplayedRainParticleCount = 0;

    float mRainEmitRate = 5000.0f;
    float mRainScale = 1.0f;
    DirectX::XMFLOAT3 mAcceleration { -1.0f, -9.8f, 0.0f };

    bool mIsWireframe = false;
    bool mDrawSkirts = true;
    float mSkirtOffsetY = 2.0f;
    int mNumSubdivisionsPerPatch = 0;
    float mMinTessDistance = 10.0f;
    float mMaxTessDistance = 150.0f;
    float mMaxTess = 6.0f;

    bool mNormalMapsEnabled = true;
    bool mReflectionsEnabled = true;
    bool mShadowsEnabled = true;

    POINT mLastMousePos;
};