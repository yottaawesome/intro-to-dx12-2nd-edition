#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Shaders/SharedTypes.h"

enum GFX_ROOT_ARG
{
    GFX_ROOT_ARG_OBJECT_CBV = 0,
    GFX_ROOT_ARG_PASS_CBV,
    GFX_ROOT_ARG_SKINNED_CBV,
    GFX_ROOT_ARG_MATERIAL_SRV,
    GFX_ROOT_ARG_INSTANCEDATA_SRV,
    GFX_ROOT_ARG_COUNT
};

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
public:
    
    FrameResource(ID3D12Device* device, UINT passCount, UINT maxInstanceCount, UINT materialCount);
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

    // NOTE: In this demo, we instance only one render-item, so we only have one structured buffer to 
    // store instancing data.  To make this more general (i.e., to support instancing multiple render-items), 
    // you would need to have a structured buffer for each render-item, and allocate each buffer with enough
    // room for the maximum number of instances you would ever draw.  
    // This sounds like a lot, but it is actually no more than the amount of per-object constant data we 
    // would need if we were not using instancing.  For example, if we were drawing 1000 objects without instancing,
    // we would create a constant buffer with enough room for a 1000 objects.  With instancing, we would just
    // create a structured buffer large enough to store the instance data for 1000 instances.  
    std::unique_ptr<UploadBuffer<InstanceData>> InstanceBuffer = nullptr;

    // Fence value to mark commands up to this fence point.  This lets us
    // check if these frame resources are still in use by the GPU.
    UINT64 Fence = 0;
};