
#include "VecAddCS.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

constexpr UINT CBV_SRV_UAV_HEAP_CAPACITY = 16384;

struct Data
{
    XMFLOAT3 v1;
    XMFLOAT2 v2;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        VecAddCS theApp(hInstance);
        if(!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch(DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

VecAddCS::VecAddCS(HINSTANCE hInstance)
    : D3DApp(hInstance)
{

}

VecAddCS::~VecAddCS()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool VecAddCS::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

    // We will upload on the direct queue for the book samples, but 
    // copy queue would be better for real game.
    mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

    LoadTextures();

    std::unique_ptr<MeshGeometry> shapeGeo = BuildShapeGeometry(md3dDevice.Get(), *mUploadBatch.get());
    if(shapeGeo != nullptr)
    {
        mGeometries[shapeGeo->Name] = std::move(shapeGeo);
    }

    BuildComputeBuffers();

    // Kick off upload work asyncronously.
    std::future<void> result = mUploadBatch->End(mCommandQueue.Get());

    // Other init work...
    BuildRootSignature();
    BuildCbvSrvUavDescriptorHeap();
    BuildShadersAndInputLayout();
    BuildMaterials();
    BuildRenderItems();
    BuildFrameResources();
    BuildPSOs();

    // Block until the upload work is complete.
    result.wait();

    BuildComputeDescriptors();
    DoComputeWork();

    return true;
}

void VecAddCS::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
}
 
void VecAddCS::OnResize()
{
    D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);
}

void VecAddCS::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);
    UpdateCamera(gt);

    // Cycle through the circular frame resource array.
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    // Has the GPU finished processing the commands of the current frame resource?
    // If not, wait until the GPU has completed commands up to this fence point.
    if(mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    //
    // Animate the lights.
    //

    mLightRotationAngle += 0.1f*gt.DeltaTime();

    XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
    for(int i = 0; i < 3; ++i)
    {
        XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
        lightDir = XMVector3TransformNormal(lightDir, R);
        XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
    }

    AnimateMaterials(gt);
    UpdatePerObjectCB(gt);
    UpdateMaterialBuffer(gt);
    UpdateMainPassCB(gt);
}

void VecAddCS::Draw(const GameTimer& gt)
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    SamplerHeap& samHeap = SamplerHeap::Get();

    UpdateImgui(gt);

    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(cmdListAlloc->Reset());

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

    ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavHeap.GetD3dHeap(), samHeap.GetD3dHeap() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetGraphicsRootSignature(mGfxRootSignature.Get());

    // Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
    // set as a root descriptor.
    auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
    mCommandList->SetGraphicsRootShaderResourceView(GFX_ROOT_ARG_MATERIAL_SRV, matBuffer->GetGPUVirtualAddress());

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear the back buffer and depth buffer.
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

    mCommandList->SetPipelineState(
        mDrawWireframe ? 
        mPSOs["opaque_wireframe"].Get() : 
        mPSOs["opaque"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    // Draw imgui UI.
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    // Done recording commands.
    ThrowIfFailed(mCommandList->Close());

    mLinearAllocator->Commit(mCommandQueue.Get());

    // Add the command list to the queue for execution.
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Swap the back and front buffers
    DXGI_PRESENT_PARAMETERS presentParams = { 0 };
    ThrowIfFailed(mSwapChain->Present1(0, 0, &presentParams));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    // Advance the fence value to mark commands up to this fence point.
    mCurrFrameResource->Fence = ++mCurrentFence;

    // Add an instruction to the command queue to set a new fence point. 
    // Because we are on the GPU timeline, the new fence point won't be 
    // set until the GPU finishes processing all the commands prior to this Signal().
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void VecAddCS::UpdateImgui(const GameTimer& gt)
{
    D3DApp::UpdateImgui(gt);

    //
    // Define a panel to render GUI elements.
    // 
    ImGui::Begin("Options");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

    ImGui::Checkbox("Wireframe", &mDrawWireframe);

    GraphicsMemoryStatistics gfxMemStats = GraphicsMemory::Get(md3dDevice.Get()).GetStatistics();
    
    if (ImGui::CollapsingHeader("VideoMemoryInfo"))
    {
        static float vidMemPollTime = 0.0f;
        vidMemPollTime += gt.DeltaTime();

        static DXGI_QUERY_VIDEO_MEMORY_INFO videoMemInfo;
        if (vidMemPollTime >= 1.0f) // poll every second
        {
            mDefaultAdapter->QueryVideoMemoryInfo(
                0, // assume single GPU
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL, // interested in local GPU memory, not shared
                &videoMemInfo);

            vidMemPollTime -= 1.0f;
        }
        
        ImGui::Text("Budget (bytes): %u", videoMemInfo.Budget);
        ImGui::Text("CurrentUsage (bytes): %u", videoMemInfo.CurrentUsage);
        ImGui::Text("AvailableForReservation (bytes): %u", videoMemInfo.AvailableForReservation);
        ImGui::Text("CurrentReservation (bytes): %u", videoMemInfo.CurrentReservation);

    }
    if (ImGui::CollapsingHeader("GraphicsMemoryStatistics"))
    {
        ImGui::Text("Bytes of memory in-flight: %u", gfxMemStats.committedMemory);
        ImGui::Text("Total bytes used: %u", gfxMemStats.totalMemory);
        ImGui::Text("Total page count: %u", gfxMemStats.totalPages);
    }

    ImGui::End();

    ImGui::Render();
}

void VecAddCS::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        SetCapture(mhMainWnd);
    }
}

void VecAddCS::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void VecAddCS::OnMouseMove(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        if((btnState & MK_LBUTTON) != 0)
        {
            // Make each pixel correspond to a quarter of a degree.
            float dx = XMConvertToRadians(0.25f*static_cast<float>(x - mLastMousePos.x));
            float dy = XMConvertToRadians(0.25f*static_cast<float>(y - mLastMousePos.y));

            // Update angles based on input to orbit camera around box.
            mTheta += dx;
            mPhi += dy;

            // Restrict the angle mPhi.
            mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
        }
        else if((btnState & MK_RBUTTON) != 0)
        {
            // Make each pixel correspond to 0.005 unit in the scene.
            float dx = 0.005f*static_cast<float>(x - mLastMousePos.x);
            float dy = 0.005f*static_cast<float>(y - mLastMousePos.y);

            // Update the camera radius based on input.
            mRadius += dx - dy;

            // Restrict the radius.
            mRadius = MathHelper::Clamp(mRadius, 3.0f, 25.0f);
        }

        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }
}

void VecAddCS::OnKeyboardInput(const GameTimer& gt)
{
}

void VecAddCS::AnimateMaterials(const GameTimer& gt)
{
}

void VecAddCS::UpdateCamera(const GameTimer& gt)
{
    // Convert Spherical to Cartesian coordinates.
    mEyePos.x = mRadius*sinf(mPhi)*cosf(mTheta);
    mEyePos.z = mRadius*sinf(mPhi)*sinf(mTheta);
    mEyePos.y = mRadius*cosf(mPhi);

    // Build the view matrix.
    XMVECTOR pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
    XMStoreFloat4x4(&mView, view);
}

void VecAddCS::UpdatePerObjectCB(const GameTimer& gt)
{
    // Update per object constants once per frame so the data can be shared across different render passes.
    for(auto& ri : mAllRitems)
    {
        XMStoreFloat4x4(&ri->ObjectConstants.gWorld, XMMatrixTranspose(XMLoadFloat4x4(&ri->World)));
        XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, XMMatrixTranspose(XMLoadFloat4x4(&ri->TexTransform)));
        ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;

        // Need to hold handle until we submit work to GPU.
        ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectConstants);
    }
}

void VecAddCS::UpdateMaterialBuffer(const GameTimer& gt)
{
    MaterialLib& matLib = MaterialLib::GetLib();

    auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
    for(auto& e : matLib.GetCollection())
    {
        // Only update the buffer data if the data has changed.  If the buffer
        // data changes, it needs to be updated for each FrameResource.
        Material* mat = e.second.get();
        if(mat->NumFramesDirty > 0)
        {
            XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

            MaterialData matData;
            matData.DiffuseAlbedo = mat->DiffuseAlbedo;
            matData.FresnelR0 = mat->FresnelR0;
            matData.Roughness = mat->Roughness;
            XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));
            matData.DiffuseMapIndex = mat->AlbedoBindlessIndex;
            matData.NormalMapIndex = mat->NormalBindlessIndex;
            matData.GlossHeightAoMapIndex = mat->GlossHeightAoBindlessIndex;

            currMaterialBuffer->CopyData(mat->MatIndex, matData);

            // Next FrameResource need to be updated too.
            mat->NumFramesDirty--;
        }
    }
}

void VecAddCS::UpdateMainPassCB(const GameTimer& gt)
{
    ZeroMemory(&mMainPassCB, sizeof(mMainPassCB));

    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    XMStoreFloat4x4(&mMainPassCB.gView, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mMainPassCB.gInvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&mMainPassCB.gProj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mMainPassCB.gInvProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&mMainPassCB.gViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mMainPassCB.gInvViewProj, XMMatrixTranspose(invViewProj));
    mMainPassCB.gEyePosW = mEyePos;
    mMainPassCB.gRenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    mMainPassCB.gInvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
    mMainPassCB.gNearZ = 1.0f;
    mMainPassCB.gFarZ = 1000.0f;
    mMainPassCB.gTotalTime = gt.TotalTime();
    mMainPassCB.gDeltaTime = gt.DeltaTime();
    mMainPassCB.gAmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };

    mMainPassCB.gNumDirLights = 3;
    mMainPassCB.gNumPointLights = 0;
    mMainPassCB.gNumSpotLights = 0;

    mMainPassCB.gLights[0].Direction = mRotatedLightDirections[0];
    mMainPassCB.gLights[0].Strength = { 0.9f, 0.8f, 0.7f };
    mMainPassCB.gLights[1].Direction = mRotatedLightDirections[1];
    mMainPassCB.gLights[1].Strength = { 0.4f, 0.4f, 0.4f };
    mMainPassCB.gLights[2].Direction = mRotatedLightDirections[2];
    mMainPassCB.gLights[2].Strength = { 0.2f, 0.2f, 0.2f };

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);
}

void VecAddCS::LoadTextures()
{
    TextureLib& texLib = TextureLib::GetLib();
    texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
}

void VecAddCS::BuildCbvSrvUavDescriptorHeap()
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

    InitImgui(cbvSrvUavHeap);

    TextureLib& texLib = TextureLib::GetLib();
    for(auto& it : texLib.GetCollection())
    {
        Texture* tex = it.second.get();
        tex->BindlessIndex = cbvSrvUavHeap.NextFreeIndex();

        CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor = cbvSrvUavHeap.CpuHandle(tex->BindlessIndex);
        ID3D12Resource* texResource = tex->Resource.Get();
        if(tex->IsCubeMap)
        {
            CreateSrvCube(md3dDevice.Get(), texResource, texResource->GetDesc().Format, texResource->GetDesc().MipLevels, hDescriptor);
        }
        else
        {
            CreateSrv2d(md3dDevice.Get(), texResource, texResource->GetDesc().Format, texResource->GetDesc().MipLevels, hDescriptor);
        }
    }
}

void VecAddCS::BuildRootSignature()
{
    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER gfxRootParameters[GFX_ROOT_ARG_COUNT];

    // Perfomance TIP: Order from most frequent to least frequent.
    gfxRootParameters[GFX_ROOT_ARG_OBJECT_CBV].InitAsConstantBufferView(0);
    gfxRootParameters[GFX_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);
    gfxRootParameters[GFX_ROOT_ARG_MATERIAL_SRV].InitAsShaderResourceView(0);

    // A root signature is an array of root parameters.
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        GFX_ROOT_ARG_COUNT,
        gfxRootParameters,
        0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    // create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(
        &rootSigDesc, 
        D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), 
        errorBlob.GetAddressOf());

    if(errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mGfxRootSignature)));

    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER computeRootParameters[COMPUTE_ROOT_ARG_COUNT];

    // Perfomance TIP: Order from most frequent to least frequent.
    computeRootParameters[COMPUTE_ROOT_ARG_DISPATCH_CBV].InitAsConstantBufferView(0);
    computeRootParameters[COMPUTE_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);

    // A root signature is an array of root parameters.
    CD3DX12_ROOT_SIGNATURE_DESC computeRootSigDesc(
        COMPUTE_ROOT_ARG_COUNT, computeRootParameters,
        0, nullptr, // static samplers
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    hr = D3D12SerializeRootSignature(&computeRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if(errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(0,
                  serializedRootSig->GetBufferPointer(),
                  serializedRootSig->GetBufferSize(),
                  IID_PPV_ARGS(mComputeRootSignature.GetAddressOf())));
}

void VecAddCS::BuildShadersAndInputLayout()
{
#if defined(DEBUG) || defined(_DEBUG)  
#define COMMA_DEBUG_ARGS ,DXC_ARG_DEBUG, DXC_ARG_SKIP_OPTIMIZATIONS
#else
#define COMMA_DEBUG_ARGS
#endif

    std::vector<LPCWSTR> vsArgs = std::vector<LPCWSTR> { L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> psArgs = std::vector<LPCWSTR> { L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> csArgs = std::vector<LPCWSTR> { L"-E", L"CS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };

    mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicTex.hlsl", vsArgs);
    mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicTex.hlsl", psArgs);
    mShaders["vecAddCS"] = d3dUtil::CompileShader(L"Shaders\\VecAdd.hlsl", csArgs);

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void VecAddCS::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc = d3dUtil::InitDefaultPso(
        mBackBufferFormat, 
        mDepthStencilFormat, 
        mInputLayout,
        mGfxRootSignature.Get(),
        mShaders["standardVS"].Get(),
        mShaders["opaquePS"].Get());

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &basePsoDesc,
        IID_PPV_ARGS(&mPSOs["opaque"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC wireframePsoDesc = basePsoDesc;
    wireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &wireframePsoDesc,
        IID_PPV_ARGS(&mPSOs["opaque_wireframe"])));

    //
    // VecAddCS
    //

    D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
    computePsoDesc.pRootSignature = mComputeRootSignature.Get();
    computePsoDesc.CS =
    {
        reinterpret_cast<BYTE*>(mShaders["vecAddCS"]->GetBufferPointer()),
        mShaders["vecAddCS"]->GetBufferSize()
    };
    computePsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&mPSOs["vecAdd"])));
}

void VecAddCS::BuildFrameResources()
{
    MaterialLib& matLib = MaterialLib::GetLib();

    constexpr UINT passCount = 1;
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(
            std::make_unique<FrameResource>(md3dDevice.Get(),
            passCount, matLib.GetMaterialCount()));
    }
}

void VecAddCS::BuildMaterials()
{
    MaterialLib::GetLib().Init(md3dDevice.Get());
}



void VecAddCS::AddRenderItem(RenderLayer layer, const DirectX::XMFLOAT4X4& world, const XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs)
{
    auto ritem = std::make_unique<RenderItem>();
    ritem->World = world;
    ritem->TexTransform = texTransform;
    ritem->Mat = mat;
    ritem->Geo = geo;
    ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    ritem->IndexCount = drawArgs.IndexCount;
    ritem->StartIndexLocation = drawArgs.StartIndexLocation;
    ritem->BaseVertexLocation = drawArgs.BaseVertexLocation;

    mRitemLayer[(int)layer].push_back(ritem.get());
    mAllRitems.push_back(std::move(ritem));
}

void VecAddCS::BuildRenderItems()
{
    MaterialLib& matLib = MaterialLib::GetLib();

    XMFLOAT4X4 worldTransform;
    XMFLOAT4X4 texTransform;

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(0.0f, 0.0f, 0.0f));
    XMStoreFloat4x4(&texTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["crate"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["box"]);
}

void VecAddCS::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();

    for(size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        cmdList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_OBJECT_CBV, ri->MemHandleToObjectCB.GpuAddress());

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}

std::unique_ptr<MeshGeometry> VecAddCS::BuildShapeGeometry(
    ID3D12Device* device,
    ResourceUploadBatch& uploadBatch)
{
    MeshGen meshGen;
    MeshGenData box = meshGen.CreateBox(1.0f, 1.0f, 1.0f, 3);

    //
    // We are concatenating all the geometry into one big vertex/index buffer.  So
    // define the regions in the buffer each submesh covers.
    //
    MeshGenData compositeMesh;
    SubmeshGeometry boxSubmesh = compositeMesh.AppendSubmesh(box);

    // Extract the vertex elements we are interested into our vertex buffer. 
    std::vector<ModelVertex> vertices(compositeMesh.Vertices.size());
    for(size_t i = 0; i < compositeMesh.Vertices.size(); ++i)
    {
        vertices[i].Pos = compositeMesh.Vertices[i].Position;
        vertices[i].Normal = compositeMesh.Vertices[i].Normal;
        vertices[i].TangentU = compositeMesh.Vertices[i].TangentU;
        vertices[i].TexC = compositeMesh.Vertices[i].TexC;
    }

    const uint32_t indexCount = (UINT)compositeMesh.Indices32.size();

    const UINT indexElementByteSize = sizeof(uint16_t);
    const UINT vbByteSize = (UINT)vertices.size() * sizeof(ModelVertex);
    const UINT ibByteSize = indexCount * indexElementByteSize;

    const byte* indexData = reinterpret_cast<byte*>(compositeMesh.GetIndices16().data());

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "shapeGeo";

    geo->VertexBufferCPU.resize(vbByteSize);
    CopyMemory(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

    geo->IndexBufferCPU.resize(ibByteSize);
    CopyMemory(geo->IndexBufferCPU.data(), indexData, ibByteSize);

    CreateStaticBuffer(device, uploadBatch,
                       vertices.data(), vertices.size(), sizeof(ModelVertex),
                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &geo->VertexBufferGPU);

    CreateStaticBuffer(device, uploadBatch,
                       indexData, indexCount, indexElementByteSize,
                       D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);

    geo->VertexByteStride = sizeof(ModelVertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    geo->DrawArgs["box"] = boxSubmesh;

    return geo;
}

void VecAddCS::BuildComputeBuffers()
{
    // Generate some data.
    std::vector<Data> dataA(NumDataElements);
    std::vector<Data> dataB(NumDataElements);
    for(int i = 0; i < NumDataElements; ++i)
    {
        float x = static_cast<float>(i);
        dataA[i].v1 = XMFLOAT3(x, x, x);
        dataA[i].v2 = XMFLOAT2(x, 0);

        dataB[i].v1 = XMFLOAT3(-x, x, 0.0f);
        dataB[i].v2 = XMFLOAT2(0, -x);
    }

    UINT64 byteSize = dataA.size()*sizeof(Data);

    // Create some buffers with initial data to be used as SRVs.
    CreateStaticBuffer(
        md3dDevice.Get(), *mUploadBatch,
        dataA.data(), dataA.size(), sizeof(Data),
        D3D12_RESOURCE_STATE_GENERIC_READ, &mInputBufferA);

    CreateStaticBuffer(
        md3dDevice.Get(), *mUploadBatch,
        dataB.data(), dataB.size(), sizeof(Data),
        D3D12_RESOURCE_STATE_GENERIC_READ, &mInputBufferB);

    // Create the buffer that will be a UAV.
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(byteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&mOutputBuffer)));

    // Create the buffer that we will read back on the CPU.
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(byteSize),
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&mReadBackBuffer)));
}

void VecAddCS::BuildComputeDescriptors()
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    mBufferIndexA = cbvSrvUavHeap.NextFreeIndex();
    mBufferIndexB = cbvSrvUavHeap.NextFreeIndex();
    mBufferOutputIndex =  cbvSrvUavHeap.NextFreeIndex();

    const UINT64 firstElement = 0;
    CreateBufferSrv(
        md3dDevice.Get(),
        firstElement,
        NumDataElements,
        sizeof(Data),
        mInputBufferA.Get(),
        cbvSrvUavHeap.CpuHandle(mBufferIndexA));

    CreateBufferSrv(
        md3dDevice.Get(),
        firstElement,
        NumDataElements,
        sizeof(Data),
        mInputBufferB.Get(),
        cbvSrvUavHeap.CpuHandle(mBufferIndexB));

    CreateBufferUav(
        md3dDevice.Get(),
        firstElement,
        NumDataElements,
        sizeof(Data),
        0, // counterOffset
        mOutputBuffer.Get(),
        nullptr, // counterResource
        cbvSrvUavHeap.CpuHandle(mBufferOutputIndex));
}

void VecAddCS::DoComputeWork()
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    SamplerHeap& samHeap = SamplerHeap::Get();

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(mDirectCmdListAlloc->Reset());

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mPSOs["vecAdd"].Get()));

    ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavHeap.GetD3dHeap(), samHeap.GetD3dHeap() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetComputeRootSignature(mComputeRootSignature.Get());

    DispatchCB dispatchConstants;
    dispatchConstants.gBufferIndexA = mBufferIndexA;
    dispatchConstants.gBufferIndexB = mBufferIndexB;
    dispatchConstants.gBufferIndexOutput = mBufferOutputIndex;

    // Need to hold handle until we submit work to GPU.
    GraphicsMemory& linearAllocator = GraphicsMemory::Get(md3dDevice.Get());
    GraphicsResource memHandle = linearAllocator.AllocateConstant(dispatchConstants);

    mCommandList->SetComputeRootConstantBufferView(
        COMPUTE_ROOT_ARG_DISPATCH_CBV,
        memHandle.GpuAddress());

    mCommandList->Dispatch(1, 1, 1);

    // Schedule to copy the data to the default buffer to the readback buffer.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mOutputBuffer.Get(),
                                  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));

    mCommandList->CopyResource(mReadBackBuffer.Get(), mOutputBuffer.Get());

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mOutputBuffer.Get(),
                                  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON));

    // Done recording commands.
    ThrowIfFailed(mCommandList->Close());

    // Add the command list to the queue for execution.
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Wait for the work to finish.
    FlushCommandQueue();

    // Map the data so we can read it on CPU.
    Data* mappedData = nullptr;
    ThrowIfFailed(mReadBackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedData)));

    std::ofstream fout("results.txt");

    for(int i = 0; i < NumDataElements; ++i)
    {
        fout << "(" << mappedData[i].v1.x << ", " << mappedData[i].v1.y << ", " << mappedData[i].v1.z <<
            ", " << mappedData[i].v2.x << ", " << mappedData[i].v2.y << ")" << std::endl;
    }

    mReadBackBuffer->Unmap(0, nullptr);
}