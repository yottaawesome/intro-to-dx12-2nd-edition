#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"

struct ObjectConstants
{
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
};

struct PassConstants
{
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
};

struct ColorVertex
{
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT4 Color;
};

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
public:
    
    FrameResource(ID3D12Device* device, UINT passCount, UINT waveVertCount);
    FrameResource(const FrameResource& rhs) = delete;
    FrameResource& operator=(const FrameResource& rhs) = delete;
    ~FrameResource();

    // We cannot reset the allocator until the GPU is done processing the commands.
    // So each frame needs their own allocator.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

    // We cannot update a buffer until the GPU is done processing the commands
    // that reference it.  So each frame needs their own buffers.
    std::unique_ptr<UploadBuffer<PassConstants>> PassCB = nullptr;

    // We cannot update a dynamic vertex buffer until the GPU is done processing
    // the commands that reference it.  So each frame needs their own.
    std::unique_ptr<UploadBuffer<ColorVertex>> WavesVB = nullptr;

    // Fence value to mark commands up to this fence point.  This lets us
    // check if these frame resources are still in use by the GPU.
    UINT64 Fence = 0;
};