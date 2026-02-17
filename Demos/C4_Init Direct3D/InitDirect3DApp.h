#pragma once

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/MeshGen.h"
#include "../../Common/DescriptorUtil.h"

class InitDirect3DApp : public D3DApp
{
public:
    InitDirect3DApp(HINSTANCE hInstance);
    InitDirect3DApp(const InitDirect3DApp& rhs) = delete;
    InitDirect3DApp& operator=(const InitDirect3DApp& rhs) = delete;
    ~InitDirect3DApp();

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

private:

    POINT mLastMousePos;
};