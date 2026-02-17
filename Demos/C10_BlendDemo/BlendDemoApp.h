#pragma once

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/MeshGen.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Common/Camera.h"
#include "../../Common/TextureLib.h"
#include "../../Common/MaterialLib.h"
#include "FrameResource.h"
#include "Waves.h"

enum class RenderLayer : int
{
    Opaque = 0,
    Transparent,
    AlphaTested,
    Debug,
    Sky,
    Count
};

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
    D3D_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

class BlendDemoApp : public D3DApp
{
public:
    BlendDemoApp(HINSTANCE hInstance);
    BlendDemoApp(const BlendDemoApp& rhs) = delete;
    BlendDemoApp& operator=(const BlendDemoApp& rhs) = delete;
    ~BlendDemoApp();

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
    void UpdateCamera(const GameTimer& gt);
    void UpdatePerObjectCB(const GameTimer& gt);
    void UpdateMaterialBuffer(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateWaves(const GameTimer& gt);

    void LoadTextures();
    void BuildCbvSrvUavDescriptorHeap();
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildMaterials();

    void AddRenderItem(
        RenderLayer layer, const DirectX::XMFLOAT4X4& world, 
        const DirectX::XMFLOAT4X4& texTransform, Material* mat,
        MeshGeometry* geo, SubmeshGeometry& drawArgs);
    void BuildRenderItems();
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);

    std::unique_ptr<MeshGeometry> BuildLandGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch);
    std::unique_ptr<MeshGeometry> BuildWaveGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch);

    float GetHillsHeight(float x, float z)const;
    DirectX::XMFLOAT3 GetHillsNormal(float x, float z)const;

private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<IDxcBlob>> mShaders;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D12PipelineState>> mPSOs;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    RenderItem* mWavesRitem = nullptr;
    std::unique_ptr<Waves> mWaves;

    PerPassCB mMainPassCB;

    DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    DirectX::XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
    float mTheta = 1.5f*DirectX::XM_PI;
    float mPhi = DirectX::XM_PIDIV2 - 0.1f;
    float mRadius = 50.0f;

    float mLightRotationAngle = 0.0f;
    DirectX::XMFLOAT3 mBaseLightDirections[3] = {
        DirectX::XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    DirectX::XMFLOAT3 mRotatedLightDirections[3];

    POINT mLastMousePos;

    DirectX::XMFLOAT4 mFogColor = { 0.6f, 0.6f, 0.6f, 1.0f };
    bool mDrawWireframe = false;
    float mWaveScale = 1.0f;
    float mWaveSpeed = 8.0f;
    float mWaveDamping = 0.1f;

    bool mFogEnabled = true;
    float mFogStart = 20.0f;
    float mFogEnd = 160.0f;
};