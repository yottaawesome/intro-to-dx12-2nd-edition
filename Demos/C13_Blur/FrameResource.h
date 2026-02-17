#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Shaders/SharedTypes.h"

//
// Define named offsets to root parameters in root signature for readability.
//

enum GFX_ROOT_ARG
{
    GFX_ROOT_ARG_OBJECT_CBV = 0,
    GFX_ROOT_ARG_PASS_CBV,
    GFX_ROOT_ARG_MATERIAL_SRV,
    GFX_ROOT_ARG_COUNT
};

enum COMPUTE_ROOT_ARG
{
    COMPUTE_ROOT_ARG_DISPATCH_CBV = 0,
    COMPUTE_ROOT_ARG_PASS_CBV,
    COMPUTE_ROOT_ARG_PASS_EXTRA_CBV,
    COMPUTE_ROOT_ARG_COUNT
};

struct WavesDispatchCB
{
    uint gBufferIndexA;
    uint gBufferIndexB;
    uint gBufferIndexOutput;
    uint DispatchCB_Pad0;
};

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
public:
    
    FrameResource(ID3D12Device* device, UINT passCount, UINT materialCount, UINT waveVertCount);
    FrameResource(const FrameResource& rhs) = delete;
    FrameResource& operator=(const FrameResource& rhs) = delete;
    ~FrameResource();

    // We cannot reset the allocator until the GPU is done processing the commands.
    // So each frame needs their own allocator.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

    // We cannot update a buffer until the GPU is done processing the commands
    // that reference it.  So each frame needs their own buffers.
    std::unique_ptr<UploadBuffer<PerPassCB>> PassCB = nullptr;
    std::unique_ptr<UploadBuffer<MaterialData>> MaterialBuffer = nullptr;

    // We cannot update a dynamic vertex buffer until the GPU is done processing
    // the commands that reference it.  So each frame needs their own.
    std::unique_ptr<UploadBuffer<ModelVertex>> WavesVB = nullptr;

    // Fence value to mark commands up to this fence point.  This lets us
    // check if these frame resources are still in use by the GPU.
    UINT64 Fence = 0;
};