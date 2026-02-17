//***************************************************************************************
// HybridRayTracer.cpp by Frank Luna (C) 2023 All Rights Reserved.
//***************************************************************************************

#include "HybridRayTracer.h"
#include "FrameResource.h"
#include "../../Common/DescriptorUtil.h"
#include "../../Shaders/SharedTypes.h"
#include "../Common/d3dApp.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;
using namespace DirectX::SimpleMath;

struct LocalRootArguments
{
    uint32_t MaterialIndex;
    uint32_t VertexBufferBindlessIndex;
    uint32_t VertexBufferOffset;
    uint32_t IndexBufferBindlessIndex;
    uint32_t IndexBufferOffset;
    XMFLOAT2 TexScale;
};

static_assert( (sizeof(LocalRootArguments) % sizeof(uint32_t)) == 0, "Expected LocalRootArguments to be evenly divisible by uint32_t.");
const uint32_t NumLocalRootConstants = sizeof(LocalRootArguments) / sizeof(uint32_t);

HybridRayTracer::HybridRayTracer(ID3D12Device5* device, ID3D12GraphicsCommandList6* cmdList, 
                                         IDxcBlob* rayTraceLibByteCode,
                                         UINT width, UINT height) :
    mdxrDevice(device),
    mdxrCmdList(cmdList)
{
    mShaderLib = d3dUtil::ByteCodeFromBlob(rayTraceLibByteCode);

    BuildGlobalRootSignature();
    BuildLocalRootSignature();
    BuildRayTraceStateObject();

    OnResize(width, height);
}

ID3D12Resource* HybridRayTracer::GetReflectionMap()const
{
    return mReflectionMap.Get();
}

UINT HybridRayTracer::GetReflectionMapUavIndex()const
{
    return mReflectionMapUavIndex;
}

UINT HybridRayTracer::GetReflectionMapSrvIndex()const
{
    return mReflectionMapSrvIndex;
}

void HybridRayTracer::OnResize(UINT newWidth, UINT newHeight)
{
    if((mWidth != newWidth) || (mHeight != newHeight))
    {
        mWidth = newWidth;
        mHeight = newHeight;

        BuildOutputTextures();

        // New resource, so we need new descriptors to that resource.
        BuildDescriptors();
    }
}

void HybridRayTracer::AddModel(const std::string& modelName, const RTModelDef& modelDef)
{
    // Each model corresponds to a BLAS
    auto it = mModels.find(modelName);
    if(it == std::end(mModels))
    {
        mModels[modelName] = modelDef;
    }
}

void HybridRayTracer::AddInstance(const std::string& modelName,
                                  const DirectX::XMFLOAT4X4& worldTransform, 
                                  DirectX::XMFLOAT2 texScale,
                                  UINT materialIndex)
{
    RTInstance inst;
    inst.ModelName = modelName;
    inst.Transform = worldTransform;
    inst.TexScale = texScale;
    inst.MaterialIndex = materialIndex;

    mInstances.push_back(inst);
}

void HybridRayTracer::ExecuteBuildAccelerationStructureCommands(ID3D12CommandQueue* commandQueue)
{
    BuildShaderBindingTables();

    std::unordered_map<std::string, AccelerationStructureBuffers> modelBlases = BuildBlases();
    for(const auto& [name, blas] : modelBlases)
    {
       mdxrCmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(blas.accelerationStructure.Get()));
    }

    AccelerationStructureBuffers tlas = BuildTlas(modelBlases);

    // Build acceleration structures on GPU and wait until it is done.
    ThrowIfFailed(mdxrCmdList->Close());
    ID3D12CommandList* commandLists[] = { mdxrCmdList };
    commandQueue->ExecuteCommandLists(ARRAYSIZE(commandLists), commandLists);

    // Need to finish building on GPU before AccelerationStructureBuffers goes out of scope.
    D3DApp::GetApp()->FlushCommandQueue();

    // Building uses intermediate resources, but we only need to save the final results for rendering.
    for(const auto& [name, blas] : modelBlases)
    {
        mModelBlases[name] = blas.accelerationStructure;
    }
    mSceneTlas = tlas.accelerationStructure;
}

void HybridRayTracer::Draw(ID3D12Resource* passCB, ID3D12Resource* matBuffer)
{
    mdxrCmdList->SetComputeRootSignature(mGlobalRootSig.Get());
    mdxrCmdList->SetComputeRootConstantBufferView(RT_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());
    mdxrCmdList->SetComputeRootShaderResourceView(RT_ROOT_ARG_MATERIAL_SRV, matBuffer->GetGPUVirtualAddress());
    mdxrCmdList->SetComputeRootShaderResourceView(RT_ROOT_ARG_ACCELERATION_STRUCT_SRV, mSceneTlas->GetGPUVirtualAddress());

    // Specify dimensions and SBT spans.
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.HitGroupTable.StartAddress = mHitGroupShaderTable->GetGPUVirtualAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = mHitGroupShaderTable->GetDesc().Width;
    dispatchDesc.HitGroupTable.StrideInBytes = mHitGroupShaderTableStrideInBytes;
    dispatchDesc.MissShaderTable.StartAddress = mMissShaderTable->GetGPUVirtualAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = mMissShaderTable->GetDesc().Width;
    dispatchDesc.MissShaderTable.StrideInBytes = mMissShaderTableStrideInBytes;
    dispatchDesc.RayGenerationShaderRecord.StartAddress = mRayGenShaderTable->GetGPUVirtualAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = mRayGenShaderTable->GetDesc().Width;
    dispatchDesc.Width = mWidth;
    dispatchDesc.Height = mHeight;
    dispatchDesc.Depth = 1;

    mdxrCmdList->SetPipelineState1(mdxrStateObject.Get());
    mdxrCmdList->DispatchRays(&dispatchDesc);
}

void HybridRayTracer::BuildOutputTextures()
{
    D3D12_RESOURCE_DESC texDesc;
    ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = mWidth;
    texDesc.Height = mHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = ReflectionMapFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ThrowIfFailed(mdxrDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&mReflectionMap)));
}

void HybridRayTracer::BuildGlobalRootSignature()
{
    //
    // Define shader parameters global to all ray-trace shaders.
    //

    CD3DX12_ROOT_PARAMETER rayTraceRootParameters[RT_ROOT_ARG_COUNT];

    rayTraceRootParameters[RT_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);
    rayTraceRootParameters[RT_ROOT_ARG_MATERIAL_SRV].InitAsShaderResourceView(0);
    rayTraceRootParameters[RT_ROOT_ARG_ACCELERATION_STRUCT_SRV].InitAsShaderResourceView(1);

    CD3DX12_ROOT_SIGNATURE_DESC rtGlobalRootSigDesc(
        RT_ROOT_ARG_COUNT, rayTraceRootParameters,
        0, nullptr, // static samplers
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(
        &rtGlobalRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if(errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(mdxrDevice->CreateRootSignature(0,
                  serializedRootSig->GetBufferPointer(),
                  serializedRootSig->GetBufferSize(),
                  IID_PPV_ARGS(mGlobalRootSig.GetAddressOf())));
}

void HybridRayTracer::BuildLocalRootSignature()
{
    //
    // Define additional "local" shader parameters whose arguments vary per shader table entry.
    // In particular, this is how we pass per-object arguments. The data would be similar to a
    // PerObjectCB, except that the world transform is not needed because it is baked into the 
    // acceleration structure already.
    // 

    const UINT numRootParams = 1;
    const UINT num32BitValues = NumLocalRootConstants; // see LocalRootArguments
    const UINT shaderRegister = 0;
    CD3DX12_ROOT_PARAMETER rayTraceRootParameters[1];
    rayTraceRootParameters[0].InitAsConstants(num32BitValues, shaderRegister);

    CD3DX12_ROOT_SIGNATURE_DESC rtLocalRootSigDesc(numRootParams, rayTraceRootParameters);
    rtLocalRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(
        &rtLocalRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if(errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(mdxrDevice->CreateRootSignature(0,
                  serializedRootSig->GetBufferPointer(),
                  serializedRootSig->GetBufferSize(),
                  IID_PPV_ARGS(mLocalRootSig.GetAddressOf())));
}

void HybridRayTracer::BuildRayTraceStateObject()
{
    //
    // A bit of boilerplate needed to configure the ray tracing pipeline.
    //

    CD3DX12_STATE_OBJECT_DESC raytracingPipeline { D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

    //
    // Set the compiled DXIL library code that contains our ray tracing shaders and define which shaders
    // to export from the library. If we omit explicit exports, all will be exported.
    //
    auto lib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    lib->SetDXILLibrary(&mShaderLib);
    lib->DefineExport(RaygenShaderName);
    lib->DefineExport(ClosestHitShaderName);
    lib->DefineExport(ColorMissShaderName);
    lib->DefineExport(ShadowMissShaderName);

    //
    // Define a hit group, which basically specifies the shaders involved with ray hits.
    // 
    // SetHitGroupExport: Give the hit group a name so we can refer to it by name in other parts of the DXR API.
    // SetHitGroupType: Either D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE or D3D12_HIT_GROUP_TYPE_TRIANGLES. 
    //                  The API has some automatic functionality for triangles. For example, there is a 
    //                  built-in ray-triangle intersection.
    // SetClosestHitShaderImport: Sets the closest hit shader for this hit group.
    // SetAnyHitShaderImport: Sets the any hit shader for this hit group.
    // SetIntersectionShaderImport: Sets the intersection shader for this hit group.
    // 
    // Note that if your ray tracing program does not use one of the hit shader types, then it does not need to set it.
    // 
    auto hitGroup = raytracingPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetHitGroupExport(HitGroupName);
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
    hitGroup->SetClosestHitShaderImport(ClosestHitShaderName);
    // hitGroup->SetAnyHitShaderImport(); Not used
    
    // 
    // Define the size of the payload and attribute structures. 
    // This is application defined and the smaller the better for performance.
    // 
    auto shaderConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    UINT payloadSize = std::max(sizeof(ColorRayPayload), sizeof(ShadowRayPayload));
    UINT attributeSize = sizeof(GeoAttributes);
    shaderConfig->Config(payloadSize, attributeSize);

    //
    // Set local root signature, and associate it with hit group.
    //
    auto localRootSignature = raytracingPipeline.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
    localRootSignature->SetRootSignature(mLocalRootSig.Get());

    auto rootSignatureAssociation = raytracingPipeline.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
    rootSignatureAssociation->SetSubobjectToAssociate(*localRootSignature);
    rootSignatureAssociation->AddExport(HitGroupName);

    //
    // Set the global root signature.
    //
    auto globalRootSignature = raytracingPipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(mGlobalRootSig.Get());

    //
    // Set max recursion depth. For internal driver optimizations, specify the lowest number you need.
    //
    auto pipelineConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    const UINT maxRecursionDepth = MAX_RECURSION_DEPTH;
    pipelineConfig->Config(maxRecursionDepth);

    ThrowIfFailed(mdxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&mdxrStateObject)));
}


HybridRayTracer::ModelBlasList HybridRayTracer::BuildBlases()
{
    constexpr uint32_t numGeometries = 1;

    D3D12_RAYTRACING_GEOMETRY_DESC geoDesc[numGeometries];

    std::unordered_map<std::string, AccelerationStructureBuffers> blasBuffers(mModels.size());

    for(const auto& [name, modelDef] : mModels)
    {
        const int indexByteOffset = modelDef.StartIndexLocation * modelDef.IndexSizeInBytes;
        const int vertexByteOffset = modelDef.BaseVertexLocation * modelDef.VertexSizeInBytes;

        geoDesc[0].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geoDesc[0].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geoDesc[0].Triangles.IndexBuffer = modelDef.IndexBuffer->GetGPUVirtualAddress() + indexByteOffset;
        geoDesc[0].Triangles.IndexCount = modelDef.IndexCount;
        geoDesc[0].Triangles.IndexFormat = modelDef.IndexFormat;
        geoDesc[0].Triangles.VertexFormat = modelDef.VertexFormat;
        geoDesc[0].Triangles.VertexCount = modelDef.VertexCount;
        geoDesc[0].Triangles.VertexBuffer.StartAddress = modelDef.VertexBuffer->GetGPUVirtualAddress() + vertexByteOffset;
        geoDesc[0].Triangles.VertexBuffer.StrideInBytes = modelDef.VertexSizeInBytes;

        // This transform (if used) is used to transform the geometry relative to its model.
        // A separate instance/world transform will be applied on top of that to transform the entire model instance.
        geoDesc[0].Triangles.Transform3x4 = 0;

        //
        // BLAS is built from N geometries.
        //
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasDesc = {};
        blasDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blasDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blasDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        blasDesc.Inputs.NumDescs = numGeometries;
        blasDesc.Inputs.pGeometryDescs = geoDesc;

        // Query some info that is device dependent for building the BLAS.
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
        mdxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&blasDesc.Inputs, &prebuildInfo);
        assert(prebuildInfo.ResultDataMaxSizeInBytes > 0);

        ComPtr<ID3D12Resource> scratch;
        AllocateUAVBuffer(mdxrDevice,
                          prebuildInfo.ScratchDataSizeInBytes,
                          &scratch,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          L"ScratchResource");

        ComPtr<ID3D12Resource> blas;
        AllocateUAVBuffer(mdxrDevice, prebuildInfo.ResultDataMaxSizeInBytes,
                          &blas,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          L"BottomLevelAccelerationStructure");

        blasDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        blasDesc.DestAccelerationStructureData = blas->GetGPUVirtualAddress();

        mdxrCmdList->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr);

        AccelerationStructureBuffers bottomLevelASBuffers;
        bottomLevelASBuffers.accelerationStructure = blas;
        bottomLevelASBuffers.scratch = scratch;
        bottomLevelASBuffers.ResultDataMaxSizeInBytes = prebuildInfo.ResultDataMaxSizeInBytes;

        blasBuffers[name] = bottomLevelASBuffers;
    }
    
    return blasBuffers;
}


ComPtr<ID3D12Resource> HybridRayTracer::BuildInstanceBuffer(ModelBlasList& modelBlases)
{
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
    instanceDescs.resize(mInstances.size());

    for(uint32_t i = 0; i < mInstances.size(); ++i)
    {
        AccelerationStructureBuffers& blas = modelBlases[mInstances[i].ModelName];
        D3D12_GPU_VIRTUAL_ADDRESS blasGpuAddress = blas.accelerationStructure->GetGPUVirtualAddress();

        instanceDescs[i].InstanceMask = 1;
        instanceDescs[i].InstanceContributionToHitGroupIndex = i * RayCount * NumGeometriesPerInstance; // instance offset for SBT
        instanceDescs[i].AccelerationStructure = blasGpuAddress;
        instanceDescs[i].InstanceID = i; // for shader SV_InstanceID
        instanceDescs[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

        XMMATRIX worldTransform = XMLoadFloat4x4(&mInstances[i].Transform);
        XMStoreFloat3x4(reinterpret_cast<XMFLOAT3X4*>(instanceDescs[i].Transform), worldTransform);
    }

    UINT64 bufferSize = static_cast<UINT64>(instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    ComPtr<ID3D12Resource> instanceBuffer;
    AllocateUploadBuffer(mdxrDevice, instanceDescs.data(), bufferSize, &instanceBuffer, L"InstanceDescs");

    return instanceBuffer;
}


AccelerationStructureBuffers HybridRayTracer::BuildTlas(ModelBlasList& modelBlases)
{
    // TLAS defines instances of BLAS structures. 

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
    tlasDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlasDesc.Inputs.NumDescs = (UINT)mInstances.size();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    mdxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&tlasDesc.Inputs, &prebuildInfo);
    assert(prebuildInfo.ResultDataMaxSizeInBytes > 0);

    ComPtr<ID3D12Resource> scratch;
    AllocateUAVBuffer(mdxrDevice, 
                      prebuildInfo.ScratchDataSizeInBytes, 
                      &scratch, 
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 
                      L"ScratchResource");

    ComPtr<ID3D12Resource> tlas;
    AllocateUAVBuffer(mdxrDevice, 
                      prebuildInfo.ResultDataMaxSizeInBytes,
                      &tlas,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 
                      L"TopLevelAccelerationStructure");

    ComPtr<ID3D12Resource> instanceBuffer = BuildInstanceBuffer(modelBlases);
    tlasDesc.Inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();

    tlasDesc.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    tlasDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();

    mdxrCmdList->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);

    AccelerationStructureBuffers tlasBuffers;
    tlasBuffers.accelerationStructure = tlas;
    tlasBuffers.instanceDesc = instanceBuffer;
    tlasBuffers.scratch = scratch;
    tlasBuffers.ResultDataMaxSizeInBytes = prebuildInfo.ResultDataMaxSizeInBytes;

    return tlasBuffers;
}

// With rasterization, we draw meshes one-by-one. We can draw the meshes with different shaders by 
// binding a different PSO like this:
//
// SetTerrainPSO();
// DrawTerrainMesh();
//
// SetStaticOpaqueMeshPSO();
// DrawStaticOpaqueMeshes();
// 
// SetParticlesPSO();
// DrawParticles();
//
// Ray tracing is more complicated because when view rays are generated, we do not know 
// which geometries they will intersect. So the ray tracing system needs to know about all 
// possible shaders that might need to be run per ray dispatch. This is specified by the shader binding table (SBT). 
//
// The SBT is just a buffer of shader records we fill out. There is an implicit agreement that it is filled out 
// correctly such that it matches how the scene and ray tracing pipeline is configured. 
// More specifically, we need to map each geometry in each instance in the scene to an entry in the shader table.
// Furthermore, we might have multiple types of rays being casted (e.g., primary and shadow rays). Thus each
// geometry will have an entry for each type of ray.
//  
// From [GPU Gems 2] the general formula is:
// 
// HG_index = I_offset + R_offset + R_stride * G_id
// HG_byteOffset = HG_stride * HG_index
//   
// where
//   
//   I_offset: Index to the starting record in the shader table for the instance.
//   R_offset: ray index from [0, RayTypeCount).
//   G_id: instance geometry index from [0, GeometryCount(instanceId)).
//   R_stride: The ray type count.
//   HG_stride: byte size between shader records
// 
// To understand the formula, it is a bit easier to start with a common example and then modify it as needed. 
// Suppose we have 3 instances, where instance 1 has 1 geometry, instances 2 and 3 and have 2 geometries, and 
// suppose we are casting two types of rays: primary and shadow. Then R_stride = 2 and R_offset in {0, 1} and 
// our shader table looks like this:
// 
// ShaderRecord shaderTable[NUM_ENTRIES];
// 
// instanceOffset0 = 0;
// ShaderRecord* instance0 = shaderTable[instanceOffset0];
//    instance0[0]: ShaderRecord for { Instance0, Geo0, Ray0 (primary) }
//    instance0[1]: ShaderRecord for { Instance0, Geo0, Ray1 (shadow) }
// 
// instanceOffset1 = 2;
// ShaderRecord* instance1 = shaderTable[instanceOffset1];
//    instance1[0]: ShaderRecord for { Instance1, Geo0, Ray0 (primary) }
//    instance1[1]: ShaderRecord for { Instance1, Geo0, Ray1 (shadow) }
//    instance1[2]: ShaderRecord for { Instance1, Geo1, Ray0 (primary) }
//    instance1[3]: ShaderRecord for { Instance1, Geo1, Ray1 (shadow) }
// 
// instanceOffset2 = 6;
// ShaderRecord* instance2 = shaderTable[instanceOffset2];
//    instance2[0]: ShaderRecord for { Instance2, Geo0, Ray0 (primary) }
//    instance2[1]: ShaderRecord for { Instance2, Geo0, Ray1 (shadow) }
//    instance2[2]: ShaderRecord for { Instance2, Geo1, Ray0 (primary) }
//    instance2[3]: ShaderRecord for { Instance2, Geo1, Ray1 (shadow) }
// 
// Note that, in general, the number of geometries per instance can vary, so we must manually specify I_offset for each instance. 
//
// There is also a "miss" shader table. However, it is much simpler because you do not need an entry per geometry.
// You only need an entry per ray type.

void HybridRayTracer::BuildShaderBindingTables()
{
    ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
    ThrowIfFailed(mdxrStateObject.As(&stateObjectProperties));

    const uint32_t shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

    //
    // Ray gen shader table
    //

    void* rayGenShaderIdentifier = stateObjectProperties->GetShaderIdentifier(RaygenShaderName);
    uint32_t numShaderRecords = 1;
    uint32_t shaderRecordSize = shaderIdentifierSize;
    ShaderTable rayGenShaderTable(mdxrDevice, numShaderRecords, shaderRecordSize, L"RayGenShaderTable");
    rayGenShaderTable.push_back(ShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize));
    mRayGenShaderTable = rayGenShaderTable.GetResource();

    //
    // Miss shader table: two entries, one for color rays and one for shadow rays.
    //

    void* colorMissShaderIdentifier = stateObjectProperties->GetShaderIdentifier(ColorMissShaderName);
    void* shadowMissShaderIdentifier = stateObjectProperties->GetShaderIdentifier(ShadowMissShaderName);
    numShaderRecords = 2;
    shaderRecordSize = shaderIdentifierSize;
    ShaderTable missShaderTable(mdxrDevice, numShaderRecords, shaderRecordSize, L"MissShaderTable");
    missShaderTable.push_back(ShaderRecord(colorMissShaderIdentifier, shaderIdentifierSize));
    missShaderTable.push_back(ShaderRecord(shadowMissShaderIdentifier, shaderIdentifierSize));
    mMissShaderTableStrideInBytes = missShaderTable.GetShaderRecordSize();
    mMissShaderTable = missShaderTable.GetResource();

    //
    // Hit group shader table
    //

    // To keep things simple, all our objects use the same hit group shaders. In general, 
    // different objects might use different hit group shaders.
    void* hitGroupShaderIdentifier = stateObjectProperties->GetShaderIdentifier(HitGroupName);

    // Again, for simplicity, we assume in this demo that each instance only has one geometry.
    numShaderRecords = RayCount * (UINT)mInstances.size();
    shaderRecordSize = shaderIdentifierSize + sizeof(LocalRootArguments);
    ShaderTable hitGroupShaderTable(mdxrDevice, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");

    for(uint32_t instanceIndex = 0; instanceIndex < mInstances.size(); ++instanceIndex)
    {
        for(uint32_t rayTypeIndex = 0; rayTypeIndex < RayCount; ++rayTypeIndex)
        {
            LocalRootArguments rootArguments;

            const RTModelDef& model = mModels[mInstances[instanceIndex].ModelName];

            rootArguments.MaterialIndex = mInstances[instanceIndex].MaterialIndex;
            rootArguments.VertexBufferBindlessIndex = model.VertexBufferBindlessIndex;
            rootArguments.VertexBufferOffset = model.BaseVertexLocation;
            rootArguments.IndexBufferBindlessIndex = model.IndexBufferBindlessIndex;
            rootArguments.IndexBufferOffset = model.StartIndexLocation;
            rootArguments.TexScale = mInstances[instanceIndex].TexScale;
            hitGroupShaderTable.push_back(ShaderRecord(
                hitGroupShaderIdentifier,
                shaderIdentifierSize,
                &rootArguments,
                sizeof(LocalRootArguments)));
        }
    }

    mHitGroupShaderTableStrideInBytes = hitGroupShaderTable.GetShaderRecordSize();
    mHitGroupShaderTable = hitGroupShaderTable.GetResource();
}

void HybridRayTracer::BuildDescriptors()
{
    CbvSrvUavHeap& heap = CbvSrvUavHeap::Get();

    mReflectionMapUavIndex = heap.NextFreeIndex();
    mReflectionMapSrvIndex = heap.NextFreeIndex();

    CreateUav2d(mdxrDevice, mReflectionMap.Get(), mFormat, 0, heap.CpuHandle(mReflectionMapUavIndex));
    CreateSrv2d(mdxrDevice, mReflectionMap.Get(), mFormat, 1, heap.CpuHandle(mReflectionMapSrvIndex));
}