#pragma once

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/MeshGen.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Common/Camera.h"


struct ObjectConstants
{
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
};

struct PassConstants
{
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
};

enum ROOT_ARG
{
    ROOT_ARG_OBJECT_CBV = 0,
    ROOT_ARG_PASS_CBV,
    ROOT_ARG_COUNT
};

inline constexpr UINT BOX_GRID_SIZE = 3;
inline constexpr UINT BOX_COUNT = BOX_GRID_SIZE*BOX_GRID_SIZE;

class BoxGridApp : public D3DApp
{
public:
    BoxGridApp(HINSTANCE hInstance);
    BoxGridApp(const BoxGridApp& rhs) = delete;
    BoxGridApp& operator=(const BoxGridApp& rhs) = delete;
    ~BoxGridApp();

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

    void BuildCbvSrvUavDescriptorHeap();
    void BuildConstantBuffers();
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildBoxGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch);
    void BuildPSO();

private:

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

    uint32_t mBoxCBHeapIndex[BOX_COUNT];
    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;

    uint32_t mPassCBHeapIndex = -1;
    std::unique_ptr<UploadBuffer<PassConstants>> mPassCB = nullptr;

    std::unique_ptr<MeshGeometry> mBoxGeo = nullptr;

    Microsoft::WRL::ComPtr<IDxcBlob> mvsByteCode = nullptr;
    Microsoft::WRL::ComPtr<IDxcBlob> mpsByteCode = nullptr;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> mSolidPSO = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mWireframePSO = nullptr;

    DirectX::XMFLOAT4X4 mWorld[BOX_COUNT];
    DirectX::XMFLOAT4X4 mView = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.25f*DirectX::XM_PI;
    float mPhi = 0.25f*DirectX::XM_PI;
    float mRadius = 15.0f;

    POINT mLastMousePos;

    bool mDrawWireframe = false;
};