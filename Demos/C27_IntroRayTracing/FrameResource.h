#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Shaders/SharedTypes.h"

enum GFX_ROOT_ARG
{
    GFX_ROOT_ARG_OBJECT_CBV = 0,
    GFX_ROOT_ARG_PASS_CBV,
    GFX_ROOT_ARG_SKINNED_CBV, // used for skinning demo
    GFX_ROOT_ARG_MATERIAL_SRV,
    GFX_ROOT_ARG_INSTANCEDATA_SRV, // used for instancing demo
    GFX_ROOT_ARG_COUNT
};

enum COMPUTE_ROOT_ARG
{
    COMPUTE_ROOT_ARG_DISPATCH_CBV = 0,
    COMPUTE_ROOT_ARG_PASS_CBV,
    COMPUTE_ROOT_ARG_PASS_EXTRA_CBV,
    COMPUTE_ROOT_ARG_COUNT
};

enum RT_ROOT_ARG
{
    RT_ROOT_ARG_PASS_CBV,
    RT_ROOT_ARG_MATERIAL_SRV,
    RT_ROOT_ARG_ACCELERATION_STRUCT_SRV,
    RT_ROOT_ARG_COUNT
};

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
public:
    
    FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT materialCount);
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

    // For debugging.
    Microsoft::WRL::ComPtr<ID3D12Resource> RainParticleCountReadbackBuffer;

    // Fence value to mark commands up to this fence point.  This lets us
    // check if these frame resources are still in use by the GPU.
    UINT64 Fence = 0;
};