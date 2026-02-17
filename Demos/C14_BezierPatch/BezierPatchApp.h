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

enum class RenderLayer : int
{
    OpaqueTess = 0,
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

    DirectX::XMFLOAT4 TessConstants = DirectX::XMFLOAT4(64.0f, 20.0f, 100.0f, 0.0f);

    // Primitive topology.
    D3D_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

class BezierPatchApp : public D3DApp
{
public:
    BezierPatchApp(HINSTANCE hInstance);
    BezierPatchApp(const BezierPatchApp& rhs) = delete;
    BezierPatchApp& operator=(const BezierPatchApp& rhs) = delete;
    ~BezierPatchApp();

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

    void LoadTextures();
    void BuildCbvSrvUavDescriptorHeap();
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    std::unique_ptr<MeshGeometry> BuildQuadPatchGeometry();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildMaterials();

    void AddRenderItem(
        RenderLayer layer, const DirectX::XMFLOAT4X4& world, 
        const DirectX::XMFLOAT4X4& texTransform, 
        Material* mat,
        MeshGeometry* geo, SubmeshGeometry& drawArgs, 
        D3D_PRIMITIVE_TOPOLOGY primType);
    void BuildRenderItems();
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);

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

    RenderItem* mPatchRitem = nullptr;

    PerPassCB mMainPassCB;

    DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    DirectX::XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };

    float mTheta = 0.7f*DirectX::XM_PI;
    float mPhi = 0.42f*DirectX::XM_PI;
    float mRadius = 30.0f;

    float mLightRotationAngle = 0.0f;
    DirectX::XMFLOAT3 mBaseLightDirections[3] = {
        DirectX::XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        DirectX::XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    DirectX::XMFLOAT3 mRotatedLightDirections[3];

    float mTessFactor = 32.0f;

    POINT mLastMousePos;
};