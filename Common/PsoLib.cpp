
#include "PsoLib.h"
#include "d3dUtil.h"
#include "ShaderLib.h"
#include <algorithm>

using namespace DirectX;

bool PsoLib::IsInitialized()const
{
    return mIsInitialized;
}

void PsoLib::Init(ID3D12Device5* device,
                  DXGI_FORMAT backBufferFormat,
                  DXGI_FORMAT depthStencilFormat,
                  DXGI_FORMAT ambientMapFormat,
                  DXGI_FORMAT screenNormalMapFormat,
                  ID3D12RootSignature* rootSig, 
                  ID3D12RootSignature* computeRootSig)
{
    ShaderLib& shaderLib = ShaderLib::GetLib();

    //
    // Input Layouts
    //

    const std::vector<D3D12_INPUT_ELEMENT_DESC> modelInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    const std::vector<D3D12_INPUT_ELEMENT_DESC> terrainInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    const std::vector<D3D12_INPUT_ELEMENT_DESC> skinnedInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "WEIGHTS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BONEINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };


    D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc;

    basePsoDesc = d3dUtil::InitDefaultPso(
        backBufferFormat, depthStencilFormat, modelInputLayout, rootSig,
        shaderLib["standardVS"], shaderLib["opaquePS"]);

    //
    // PSO for opaque objects.
    //

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc = basePsoDesc;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueWireframePsoDesc = opaquePsoDesc;
    opaqueWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueWireframePsoDesc, IID_PPV_ARGS(&mPSOs["opaque_wireframe"])));

    // Note: Because for SSAO we do a separate depth prepass, when we draw the main opaque pass, 
    // we can change the depth test to EQUAL.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueWithPrepassPsoDesc = basePsoDesc;
    opaqueWithPrepassPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    opaqueWithPrepassPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueWithPrepassPsoDesc, IID_PPV_ARGS(&mPSOs["opaque_wprepass"])));

    //
    // PSO for opaque skinned objects.
    //

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueSkinnedPsoDesc = basePsoDesc;
    opaqueSkinnedPsoDesc.InputLayout = { skinnedInputLayout.data(), (UINT)skinnedInputLayout.size() };
    opaqueSkinnedPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["skinnedVS"]);
    opaqueSkinnedPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["opaquePS"]);
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueSkinnedPsoDesc, IID_PPV_ARGS(&mPSOs["skinnedOpaque"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueSkinnedWireframePsoDesc = opaqueSkinnedPsoDesc;
    opaqueSkinnedWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueSkinnedWireframePsoDesc, IID_PPV_ARGS(&mPSOs["skinnedOpaque_wireframe"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueSkinnedWithPrePassPsoDesc = opaqueSkinnedPsoDesc;
    opaqueSkinnedWithPrePassPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    opaqueSkinnedWithPrePassPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueSkinnedWithPrePassPsoDesc, IID_PPV_ARGS(&mPSOs["skinnedOpaque_wprepass"])));

    //
    // PSO for opaque instanced objects.
    //

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueInstancedPsoDesc = basePsoDesc;
    opaqueInstancedPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["instancedStandardVS"]);
    opaqueInstancedPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["instancedOpaquePS"]);
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueInstancedPsoDesc, IID_PPV_ARGS(&mPSOs["opaque_instanced"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueInstancedWireframePsoDesc = opaqueInstancedPsoDesc;
    opaqueInstancedWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueInstancedWireframePsoDesc, IID_PPV_ARGS(&mPSOs["opaque_instanced_wireframe"])));


    //
    // PSO for opaque tessellated objects.
    //

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueTessPsoDesc = basePsoDesc;
    opaqueTessPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["tessellatedVS"]);
    opaqueTessPsoDesc.HS = d3dUtil::ByteCodeFromBlob(shaderLib["tessellatedHS"]);
    opaqueTessPsoDesc.DS = d3dUtil::ByteCodeFromBlob(shaderLib["tessellatedDS"]);
    opaqueTessPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["opaquePS"]);
    opaqueTessPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueTessPsoDesc, IID_PPV_ARGS(&mPSOs["opaque_tess"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueTessWireframePsoDesc = opaqueTessPsoDesc;
    opaqueTessWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueTessWireframePsoDesc, IID_PPV_ARGS(&mPSOs["opaque_tess_wireframe"])));

    //
    // PSO for highlight objects (used in picking demo).
    //

    D3D12_GRAPHICS_PIPELINE_STATE_DESC highlightPsoDesc = opaquePsoDesc;

    // Change the depth test from < to <= so that if we draw the same triangle twice, it will
    // still pass the depth test.  This is needed because we redraw the picked triangle with a
    // different material to highlight it.  If we do not use <=, the triangle will fail the 
    // depth test the 2nd time we try and draw it.
    highlightPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // Standard transparency blending.
    D3D12_RENDER_TARGET_BLEND_DESC highlightBlendDesc;
    highlightBlendDesc.BlendEnable = true;
    highlightBlendDesc.LogicOpEnable = false;
    highlightBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    highlightBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    highlightBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    highlightBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    highlightBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    highlightBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    highlightBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    highlightBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    highlightPsoDesc.BlendState.RenderTarget[0] = highlightBlendDesc;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&highlightPsoDesc, IID_PPV_ARGS(&mPSOs["highlight"])));
    
    //
    // PSO for shadow map pass.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC smapPsoDesc = basePsoDesc;
    smapPsoDesc.RasterizerState.DepthBias = 10000;//100000;
    smapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    smapPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;
    smapPsoDesc.pRootSignature = rootSig;
    smapPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["shadowVS"]);
    smapPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["shadowOpaquePS"]);
    smapPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN; // depth pass only
    smapPsoDesc.NumRenderTargets = 0;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&smapPsoDesc, IID_PPV_ARGS(&mPSOs["shadow_opaque"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC skinnedSmapPsoDesc = smapPsoDesc;
    skinnedSmapPsoDesc.InputLayout = { skinnedInputLayout.data(), (UINT)skinnedInputLayout.size() };
    skinnedSmapPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["skinnedShadowVS"]);
    skinnedSmapPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["shadowOpaquePS"]);
    ThrowIfFailed(device->CreateGraphicsPipelineState(&skinnedSmapPsoDesc, IID_PPV_ARGS(&mPSOs["skinnedShadow_opaque"])));

    //
    // PSO for debug layer.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC debugPsoDesc = basePsoDesc;
    debugPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["debugVS"]);
    debugPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["debugPS"]);
    ThrowIfFailed(device->CreateGraphicsPipelineState(&debugPsoDesc, IID_PPV_ARGS(&mPSOs["debug"])));

    //
    // PSO for drawing normals.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC drawViewNormalsPsoDesc = basePsoDesc;
    drawViewNormalsPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["drawNormalsVS"]);
    drawViewNormalsPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawViewNormalsPS"]);
    drawViewNormalsPsoDesc.RTVFormats[0] = screenNormalMapFormat;
    drawViewNormalsPsoDesc.SampleDesc.Count = 1;
    drawViewNormalsPsoDesc.SampleDesc.Quality = 0;
    drawViewNormalsPsoDesc.DSVFormat = depthStencilFormat;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&drawViewNormalsPsoDesc, IID_PPV_ARGS(&mPSOs["drawViewNormals"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC drawSkinnedViewNormalsPsoDesc = drawViewNormalsPsoDesc;
    drawSkinnedViewNormalsPsoDesc.InputLayout = { skinnedInputLayout.data(), (UINT)skinnedInputLayout.size() };
    drawSkinnedViewNormalsPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["drawSkinnedNormalsVS"]);
    drawSkinnedViewNormalsPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawViewNormalsPS"]);
    ThrowIfFailed(device->CreateGraphicsPipelineState(&drawSkinnedViewNormalsPsoDesc, IID_PPV_ARGS(&mPSOs["drawSkinnedViewNormals"])));

    //
    // PSO for drawing normals.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC drawBumpedWorldNormalsPsoDesc = basePsoDesc;
    drawBumpedWorldNormalsPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["drawNormalsVS"]);
    drawBumpedWorldNormalsPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawBumpedWorldNormalsPS"]);
    drawBumpedWorldNormalsPsoDesc.RTVFormats[0] = SceneNormalMapFormat;
    drawBumpedWorldNormalsPsoDesc.SampleDesc.Count = 1;
    drawBumpedWorldNormalsPsoDesc.SampleDesc.Quality = 0;
    drawBumpedWorldNormalsPsoDesc.DSVFormat = depthStencilFormat;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&drawBumpedWorldNormalsPsoDesc, IID_PPV_ARGS(&mPSOs["drawBumpedWorldNormals"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC drawSkinnedBumpedWorldNormalsPsoDesc = drawBumpedWorldNormalsPsoDesc;
    drawSkinnedBumpedWorldNormalsPsoDesc.InputLayout = { skinnedInputLayout.data(), (UINT)skinnedInputLayout.size() };
    drawSkinnedBumpedWorldNormalsPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["drawSkinnedNormalsVS"]);
    drawSkinnedBumpedWorldNormalsPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawBumpedWorldNormalsPS"]);
    ThrowIfFailed(device->CreateGraphicsPipelineState(&drawSkinnedBumpedWorldNormalsPsoDesc, IID_PPV_ARGS(&mPSOs["drawSkinnedBumpedWorldNormals"])));

    //
    // PSO for SSAO.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC ssaoPsoDesc = basePsoDesc;
    ssaoPsoDesc.InputLayout = { nullptr, 0 };
    ssaoPsoDesc.pRootSignature = rootSig;
    ssaoPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["ssaoVS"]);
    ssaoPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["ssaoPS"]);

    // SSAO effect does not need the depth buffer.
    ssaoPsoDesc.DepthStencilState.DepthEnable = false;
    ssaoPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ssaoPsoDesc.RTVFormats[0] = ambientMapFormat;
    ssaoPsoDesc.SampleDesc.Count = 1;
    ssaoPsoDesc.SampleDesc.Quality = 0;
    ssaoPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&ssaoPsoDesc, IID_PPV_ARGS(&mPSOs["ssao"])));

    //
    // PSO for SSAO blur.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC ssaoBlurPsoDesc = ssaoPsoDesc;
    ssaoBlurPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["ssaoBlurVS"]);
    ssaoBlurPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["ssaoBlurPS"]);
    ThrowIfFailed(device->CreateGraphicsPipelineState(&ssaoBlurPsoDesc, IID_PPV_ARGS(&mPSOs["ssaoBlur"])));

    //
    // PSO for sky.
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = basePsoDesc;

    // The camera is inside the sky sphere, so just turn off culling.
    skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // Make sure the depth function is LESS_EQUAL and not just LESS.  
    // Otherwise, the normalized depth values at z = 1 (NDC) will 
    // fail the depth test if the depth buffer was cleared to 1.
    skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    skyPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["skyVS"]);
    skyPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["skyPS"]);
    ThrowIfFailed(device->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOs["sky"])));

    //
    // PSO for terrain.
    // 
    D3D12_GRAPHICS_PIPELINE_STATE_DESC terrainPsoDesc = opaquePsoDesc;

    terrainPsoDesc.InputLayout = { terrainInputLayout.data(), (UINT)terrainInputLayout.size() };
    terrainPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    terrainPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainVS"]);
    terrainPsoDesc.HS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainHS"]);
    terrainPsoDesc.DS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainDS"]);
    terrainPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainPS"]);
    ThrowIfFailed(device->CreateGraphicsPipelineState(&terrainPsoDesc, IID_PPV_ARGS(&mPSOs["terrain"])));

    terrainPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&terrainPsoDesc, IID_PPV_ARGS(&mPSOs["terrain_wireframe"])));

    CD3DX12_RASTERIZER_DESC terrainRasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    terrainRasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    terrainRasterizerDesc.DepthBias = 10000;
    terrainRasterizerDesc.DepthBiasClamp = 0.0f;
    terrainRasterizerDesc.SlopeScaledDepthBias = 1.0f;

    terrainPsoDesc = smapPsoDesc;
    terrainPsoDesc.RasterizerState = terrainRasterizerDesc;
    terrainPsoDesc.InputLayout = { terrainInputLayout.data(), (UINT)terrainInputLayout.size() };
    terrainPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    terrainPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainShadowVS"]);
    terrainPsoDesc.HS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainShadowHS"]);
    terrainPsoDesc.DS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainShadowDS"]);
    terrainPsoDesc.PS = D3D12_SHADER_BYTECODE { nullptr, 0 };
    ThrowIfFailed(device->CreateGraphicsPipelineState(&terrainPsoDesc, IID_PPV_ARGS(&mPSOs["terrain_shadow"])));

    //
    // PSOs for particles.
    //

    if(computeRootSig != nullptr)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC updateParticlesPsoDesc = {};
        updateParticlesPsoDesc.pRootSignature = computeRootSig;
        updateParticlesPsoDesc.CS = d3dUtil::ByteCodeFromBlob(shaderLib["updateParticlesCS"]);
        updateParticlesPsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        updateParticlesPsoDesc.NodeMask = 0;
        ThrowIfFailed(device->CreateComputePipelineState(&updateParticlesPsoDesc, IID_PPV_ARGS(&mPSOs["updateParticles"])));

        D3D12_COMPUTE_PIPELINE_STATE_DESC emitParticlesPsoDesc = {};
        emitParticlesPsoDesc.pRootSignature = computeRootSig;
        emitParticlesPsoDesc.CS = d3dUtil::ByteCodeFromBlob(shaderLib["emitParticlesCS"]);
        emitParticlesPsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        ThrowIfFailed(device->CreateComputePipelineState(&emitParticlesPsoDesc, IID_PPV_ARGS(&mPSOs["emitParticles"])));

        D3D12_COMPUTE_PIPELINE_STATE_DESC postUpdateParticlesPsoDesc = {};
        postUpdateParticlesPsoDesc.pRootSignature = computeRootSig;
        postUpdateParticlesPsoDesc.CS = d3dUtil::ByteCodeFromBlob(shaderLib["postUpdateParticlesCS"]);
        postUpdateParticlesPsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        ThrowIfFailed(device->CreateComputePipelineState(&postUpdateParticlesPsoDesc, IID_PPV_ARGS(&mPSOs["postUpdateParticles"])));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC drawParticlesPsoDesc = opaquePsoDesc;
        drawParticlesPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3D12_RENDER_TARGET_BLEND_DESC particlesAddBlendDesc;
        particlesAddBlendDesc.BlendEnable = true;
        particlesAddBlendDesc.LogicOpEnable = false;
        particlesAddBlendDesc.SrcBlend = D3D12_BLEND_ONE;
        particlesAddBlendDesc.DestBlend = D3D12_BLEND_ONE;
        particlesAddBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        particlesAddBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        particlesAddBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
        particlesAddBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        particlesAddBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
        particlesAddBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        drawParticlesPsoDesc.BlendState.RenderTarget[0] = particlesAddBlendDesc;
        drawParticlesPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["drawParticlesVS"]);
        drawParticlesPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawParticlesAddBlendPS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&drawParticlesPsoDesc, IID_PPV_ARGS(&mPSOs["drawParticlesAddBlend"])));

        D3D12_RENDER_TARGET_BLEND_DESC particlesTransparencyBlendDesc = particlesAddBlendDesc;
        particlesTransparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        particlesTransparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        drawParticlesPsoDesc.BlendState.RenderTarget[0] = particlesTransparencyBlendDesc;
        drawParticlesPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["drawParticlesTransparencyBlendPS"]);
        ThrowIfFailed(device->CreateGraphicsPipelineState(&drawParticlesPsoDesc, IID_PPV_ARGS(&mPSOs["drawParticlesTransparencyBlend"])));
    }

    InitHelixParticleMeshShaderPSOs(
        device,
        backBufferFormat,
        depthStencilFormat,
        rootSig);

    InitTerrainMeshShaderPSOs(
        device,
        backBufferFormat,
        depthStencilFormat,
        rootSig);

    //
    // RT hybrid composite ray traced reflections
    // 

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueHybridRTPsoDesc = basePsoDesc;
    opaqueHybridRTPsoDesc.VS = d3dUtil::ByteCodeFromBlob(shaderLib["opaqueHybridRT_vs"]);
    opaqueHybridRTPsoDesc.PS = d3dUtil::ByteCodeFromBlob(shaderLib["opaqueHybridRT_ps"]);
    opaqueHybridRTPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    opaqueHybridRTPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&opaqueHybridRTPsoDesc, IID_PPV_ARGS(&mPSOs["opaque_hybrid_rt"])));

    mIsInitialized = true;
}

bool PsoLib::AddPso(const std::string& name, Microsoft::WRL::ComPtr<ID3D12PipelineState> pso)
{
    if(mPSOs.find(name) == mPSOs.end())
    {
        mPSOs[name] = pso;
        return true;
    }

    return false;
}

ID3D12PipelineState* PsoLib::operator[](const std::string& name)
{
    if(mPSOs.find(name) != mPSOs.end())
    {
        return mPSOs[name].Get();
    }

    return nullptr;
}

void PsoLib::InitHelixParticleMeshShaderPSOs(
    ID3D12Device5* device,
    DXGI_FORMAT backBufferFormat,
    DXGI_FORMAT depthStencilFormat,
    ID3D12RootSignature* rootSig)
{
    ShaderLib& shaderLib = ShaderLib::GetLib();

    // Define the members we are interested in setting. Non-specified PSO properties will use defaults.
    struct ParticlesPsoStream
    {
        CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE pRootSignature;
        CD3DX12_PIPELINE_STATE_STREAM_MS MS;
        CD3DX12_PIPELINE_STATE_STREAM_PS PS;
        CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
        CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT DSVFormat;
        CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC BlendDesc;
        CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL DepthStencilDesc;
    };

    DXGI_FORMAT particlesPsoFormats[8] =
    {
        backBufferFormat,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
    };

    D3D12_RENDER_TARGET_BLEND_DESC particlesAddBlendDesc;
    particlesAddBlendDesc.BlendEnable = true;
    particlesAddBlendDesc.LogicOpEnable = false;
    particlesAddBlendDesc.SrcBlend = D3D12_BLEND_ONE;
    particlesAddBlendDesc.DestBlend = D3D12_BLEND_ONE;
    particlesAddBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    particlesAddBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    particlesAddBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    particlesAddBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    particlesAddBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    particlesAddBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    CD3DX12_BLEND_DESC blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    blendDesc.RenderTarget[0] = particlesAddBlendDesc;

    CD3DX12_DEPTH_STENCIL_DESC depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    ParticlesPsoStream psoStream;
    psoStream.pRootSignature = rootSig;
    psoStream.MS = d3dUtil::ByteCodeFromBlob(shaderLib["helixParticlesMS"]);
    psoStream.PS = d3dUtil::ByteCodeFromBlob(shaderLib["helixParticlesPS"]);
    psoStream.RTVFormats = CD3DX12_RT_FORMAT_ARRAY(particlesPsoFormats, 1);
    psoStream.DSVFormat = depthStencilFormat;
    psoStream.BlendDesc = blendDesc;
    psoStream.DepthStencilDesc = depthStencilDesc;

    D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = {};
    streamDesc.pPipelineStateSubobjectStream = &psoStream;
    streamDesc.SizeInBytes = sizeof(ParticlesPsoStream);

    ThrowIfFailed(device->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&mPSOs["helixParticles_ms"])));
}

void PsoLib::InitTerrainMeshShaderPSOs(
    ID3D12Device5* device,
    DXGI_FORMAT backBufferFormat,
    DXGI_FORMAT depthStencilFormat,
    ID3D12RootSignature* rootSig)
{
    ShaderLib& shaderLib = ShaderLib::GetLib();

    // Define the members we are interested in setting. Non-specified PSO properties will use defaults.
    struct TerrainPsoStream
    {
        CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE pRootSignature;
        CD3DX12_PIPELINE_STATE_STREAM_AS AS;
        CD3DX12_PIPELINE_STATE_STREAM_MS MS;
        CD3DX12_PIPELINE_STATE_STREAM_PS PS;
        CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
        CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT DSVFormat;
        CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER RasterizerState;
    };

    DXGI_FORMAT terrainPsoFormats[8] =
    {
        backBufferFormat,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN,
    };

    CD3DX12_RASTERIZER_DESC terrainRasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

    TerrainPsoStream terrainPsoStream;
    terrainPsoStream.pRootSignature = rootSig;
    terrainPsoStream.AS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainAS"]);
    terrainPsoStream.MS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainMS"]);
    terrainPsoStream.PS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainPS"]);
    terrainPsoStream.RTVFormats = CD3DX12_RT_FORMAT_ARRAY(terrainPsoFormats, 1);
    terrainPsoStream.DSVFormat = depthStencilFormat;
    terrainPsoStream.RasterizerState = terrainRasterizerDesc;

    D3D12_PIPELINE_STATE_STREAM_DESC terrainStreamDesc = {};
    terrainStreamDesc.pPipelineStateSubobjectStream = &terrainPsoStream;
    terrainStreamDesc.SizeInBytes = sizeof(TerrainPsoStream);

    ThrowIfFailed(device->CreatePipelineState(&terrainStreamDesc, IID_PPV_ARGS(&mPSOs["terrain_ms"])));

    terrainRasterizerDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    terrainPsoStream.RasterizerState = terrainRasterizerDesc;
    ThrowIfFailed(device->CreatePipelineState(&terrainStreamDesc, IID_PPV_ARGS(&mPSOs["terrain_ms_wireframe"])));

    terrainPsoFormats[0] = DXGI_FORMAT_UNKNOWN;

    terrainRasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    terrainRasterizerDesc.DepthBias = 10000;
    terrainRasterizerDesc.DepthBiasClamp = 0.0f;
    terrainRasterizerDesc.SlopeScaledDepthBias = 1.0f;

    TerrainPsoStream shadowTerrainPsoStream;
    shadowTerrainPsoStream.pRootSignature = rootSig;
    shadowTerrainPsoStream.AS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainAS"]);
    shadowTerrainPsoStream.MS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainShadowMS"]);
    shadowTerrainPsoStream.PS = D3D12_SHADER_BYTECODE { nullptr, 0 };
    shadowTerrainPsoStream.RTVFormats = CD3DX12_RT_FORMAT_ARRAY(terrainPsoFormats, 0);
    shadowTerrainPsoStream.DSVFormat = depthStencilFormat;
    shadowTerrainPsoStream.RasterizerState = terrainRasterizerDesc;

    D3D12_PIPELINE_STATE_STREAM_DESC terrainShadowStreamDesc = {};
    terrainShadowStreamDesc.pPipelineStateSubobjectStream = &shadowTerrainPsoStream;
    terrainShadowStreamDesc.SizeInBytes = sizeof(TerrainPsoStream);

    ThrowIfFailed(device->CreatePipelineState(&terrainShadowStreamDesc, IID_PPV_ARGS(&mPSOs["terrain_ms_shadow"])));

    terrainPsoFormats[0] = backBufferFormat;

    TerrainPsoStream terrainSkirtPsoStream;
    terrainSkirtPsoStream.pRootSignature = rootSig;
    terrainSkirtPsoStream.AS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainSkirtAS"]);
    terrainSkirtPsoStream.MS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainSkirtMS"]);
    terrainSkirtPsoStream.PS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainPS"]);
    terrainSkirtPsoStream.RTVFormats = CD3DX12_RT_FORMAT_ARRAY(terrainPsoFormats, 1);
    terrainSkirtPsoStream.DSVFormat = depthStencilFormat;
    terrainSkirtPsoStream.RasterizerState = terrainRasterizerDesc;

    D3D12_PIPELINE_STATE_STREAM_DESC terrainSkirtStreamDesc = {};
    terrainSkirtStreamDesc.pPipelineStateSubobjectStream = &terrainSkirtPsoStream;
    terrainSkirtStreamDesc.SizeInBytes = sizeof(TerrainPsoStream);

    ThrowIfFailed(device->CreatePipelineState(&terrainSkirtStreamDesc, IID_PPV_ARGS(&mPSOs["terrain_ms_skirt"])));

    terrainRasterizerDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    terrainSkirtPsoStream.RasterizerState = terrainRasterizerDesc;
    ThrowIfFailed(device->CreatePipelineState(&terrainSkirtStreamDesc, IID_PPV_ARGS(&mPSOs["terrain_ms_skirt_wireframe"])));

    terrainPsoFormats[0] = DXGI_FORMAT_UNKNOWN;

    terrainRasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    terrainRasterizerDesc.DepthBias = 10000;
    terrainRasterizerDesc.DepthBiasClamp = 0.0f;
    terrainRasterizerDesc.SlopeScaledDepthBias = 1.0f;

    TerrainPsoStream shadowTerrainSkirtPsoStream;
    shadowTerrainSkirtPsoStream.pRootSignature = rootSig;
    shadowTerrainSkirtPsoStream.AS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainSkirtAS"]);
    shadowTerrainSkirtPsoStream.MS = d3dUtil::ByteCodeFromBlob(shaderLib["terrainSkirtShadowMS"]);
    shadowTerrainSkirtPsoStream.PS = D3D12_SHADER_BYTECODE { nullptr, 0 };
    shadowTerrainSkirtPsoStream.RTVFormats = CD3DX12_RT_FORMAT_ARRAY(terrainPsoFormats, 0);
    shadowTerrainSkirtPsoStream.DSVFormat = depthStencilFormat;
    shadowTerrainSkirtPsoStream.RasterizerState = terrainRasterizerDesc;

    D3D12_PIPELINE_STATE_STREAM_DESC terrainShadowSkirtStreamDesc = {};
    terrainShadowSkirtStreamDesc.pPipelineStateSubobjectStream = &shadowTerrainSkirtPsoStream;
    terrainShadowSkirtStreamDesc.SizeInBytes = sizeof(TerrainPsoStream);

    ThrowIfFailed(device->CreatePipelineState(&terrainShadowSkirtStreamDesc, IID_PPV_ARGS(&mPSOs["terrain_ms_skirt_shadow"])));
}