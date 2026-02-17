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

    // Per-object tessellation controls. Only used in displacement mapping demo.
    float MeshMinTessDist = 0.0f;
    float MeshMaxTessDist = 0.0f;
    float MeshMinTess = 0.0f;
    float MeshMaxTess = 0.0f;

    PerObjectCB ObjectConstants;

    // Handle to memory in linear allocator.
    DirectX::GraphicsResource MemHandleToObjectCB;

    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;

    // Primitive topology.
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

enum class RenderLayer : int
{
    Opaque = 0,
    OpaqueTess,
    Debug,
    Sky,
    Count
};


class DisplacementMappingApp : public D3DApp
{
public:
    DisplacementMappingApp(HINSTANCE hInstance);
    DisplacementMappingApp(const DisplacementMappingApp& rhs) = delete;
    DisplacementMappingApp& operator=(const DisplacementMappingApp& rhs) = delete;
    ~DisplacementMappingApp();

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
    void UpdateMainPassCB(const GameTimer& gt);

    void LoadTextures();
    void LoadGeometry();
    void BuildRootSignature();
    void BuildCbvSrvUavDescriptorHeap();
    void BuildShadersAndInputLayout();
    void BuildPSOs();
    void BuildFrameResources();

    void BuildMaterials();

    void AddRenderItem(
        RenderLayer layer, 
        const DirectX::XMFLOAT4X4& world, 
        const DirectX::XMFLOAT4X4& texTransform, 
        Material* mat, MeshGeometry* geo, 
        SubmeshGeometry& drawArgs);

    void AddDisplacementMappedRenderItem(
        RenderLayer layer,
        const DirectX::XMFLOAT4X4& world,
        const DirectX::XMFLOAT4X4& texTransform,
        float meshMinTessDist,
        float meshMaxTessDist,
        float meshMinTess,
        float meshMaxTess,
        Material* mat, MeshGeometry* geo,
        SubmeshGeometry& drawArgs);

    void BuildRenderItems();

    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);

private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSignature = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    uint32_t mRandomTexBindlessIndex = -1;
    uint32_t mSkyBindlessIndex = -1;

    UINT mNullCubeSrvIndex = 0;
    UINT mNullTexSrvIndex = 0;

    CD3DX12_GPU_DESCRIPTOR_HANDLE mNullSrv;

    PerPassCB mMainPassCB;  

    Camera mCamera;

    

    float mLightRotationAngle = 0.0f;
    DirectX::XMFLOAT3 mBaseLightDirections[3] = {
        DirectX::XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    DirectX::XMFLOAT3 mRotatedLightDirections[3];

    bool mDrawWireframe = false;
    bool mNormalMapsEnabled = true;
    bool mReflectionsEnabled = true;

    // Tessellation parameters could vary per-object, but we keep uniform in the demo.
    float mMaxTess = 6.0f;
    float mMinTessDistance = 2;
    float mMaxTessDistance = 30;
    float mDisplacementScale = 0.1f;

    POINT mLastMousePos;
};