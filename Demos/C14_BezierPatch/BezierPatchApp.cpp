
#include "BezierPatchApp.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

constexpr UINT CBV_SRV_UAV_HEAP_CAPACITY = 16384;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        BezierPatchApp theApp(hInstance);
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

BezierPatchApp::BezierPatchApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{

}

BezierPatchApp::~BezierPatchApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool BezierPatchApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

    // We will upload on the direct queue for the book samples, but 
    // copy queue would be better for real game.
    mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

    LoadTextures();

    std::unique_ptr<MeshGeometry> quadPatchGeo = BuildQuadPatchGeometry();
    if(quadPatchGeo != nullptr)
    {
        mGeometries[quadPatchGeo->Name] = std::move(quadPatchGeo);
    }

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

    return true;
}

void BezierPatchApp::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
}
 
void BezierPatchApp::OnResize()
{
    D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);
}

void BezierPatchApp::Update(const GameTimer& gt)
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

void BezierPatchApp::Draw(const GameTimer& gt)
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

    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

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
    float clearColor[4];
    CopyMemory(clearColor, Colors::LightSteelBlue, sizeof(float)*4);

    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), clearColor, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

    mCommandList->SetPipelineState(
        mPSOs["opaque_tess"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::OpaqueTess]);

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

void BezierPatchApp::UpdateImgui(const GameTimer& gt)
{
    D3DApp::UpdateImgui(gt);

    //
    // Define a panel to render GUI elements.
    // 
    ImGui::Begin("Options");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

    ImGui::SliderFloat("Tess Factor", &mTessFactor, 1.0f, 64.0f);

    mPatchRitem->TessConstants.x = mTessFactor;

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

void BezierPatchApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        SetCapture(mhMainWnd);
    }
}

void BezierPatchApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void BezierPatchApp::OnMouseMove(WPARAM btnState, int x, int y)
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
            float dx = 0.05f*static_cast<float>(x - mLastMousePos.x);
            float dy = 0.05f*static_cast<float>(y - mLastMousePos.y);

            // Update the camera radius based on input.
            mRadius += dx - dy;

            // Restrict the radius.
            mRadius = MathHelper::Clamp(mRadius, 5.0f, 150.0f);
        }

        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }
}

void BezierPatchApp::OnKeyboardInput(const GameTimer& gt)
{
}

void BezierPatchApp::AnimateMaterials(const GameTimer& gt)
{
    MaterialLib& matLib = MaterialLib::GetLib();

    // Scroll the water material texture coordinates.
    auto waterMat = matLib["water"];

    float& tu = waterMat->MatTransform(3, 0);
    float& tv = waterMat->MatTransform(3, 1);

    tu += 0.1f * gt.DeltaTime();
    tv += 0.02f * gt.DeltaTime();

    if(tu >= 1.0f)
        tu -= 1.0f;

    if(tv >= 1.0f)
        tv -= 1.0f;

    waterMat->MatTransform(3, 0) = tu;
    waterMat->MatTransform(3, 1) = tv;

    // Material has changed, so need to update cbuffer.
    waterMat->NumFramesDirty = gNumFrameResources;
}

void BezierPatchApp::UpdateCamera(const GameTimer& gt)
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

void BezierPatchApp::UpdatePerObjectCB(const GameTimer& gt)
{
    // Update per object constants once per frame so the data can be shared across different render passes.
    for(auto& ri : mAllRitems)
    {
        XMStoreFloat4x4(&ri->ObjectConstants.gWorld, XMMatrixTranspose(XMLoadFloat4x4(&ri->World)));
        XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, XMMatrixTranspose(XMLoadFloat4x4(&ri->TexTransform)));
        ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;
        ri->ObjectConstants.gMiscFloat4 = ri->TessConstants;

        // Need to hold handle until we submit work to GPU.
        ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectConstants);
    }
}

void BezierPatchApp::UpdateMaterialBuffer(const GameTimer& gt)
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

void BezierPatchApp::UpdateMainPassCB(const GameTimer& gt)
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

    mMainPassCB.gFogColor = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    mMainPassCB.gFogStart = 0.0f;
    mMainPassCB.gFogRange = 0.0f;
    mMainPassCB.gFogEnabled = false;

    mMainPassCB.gNumDirLights = 3;
    mMainPassCB.gNumPointLights = 0;
    mMainPassCB.gNumSpotLights = 0;
    mMainPassCB.gLights[0].Direction = mRotatedLightDirections[0];
    mMainPassCB.gLights[0].Strength = { 0.8f, 0.75f, 0.7f };
    mMainPassCB.gLights[1].Direction = mRotatedLightDirections[1];
    mMainPassCB.gLights[1].Strength = { 0.3f, 0.3f, 0.3f };
    mMainPassCB.gLights[2].Direction = mRotatedLightDirections[2];
    mMainPassCB.gLights[2].Strength = { 0.2f, 0.2f, 0.2f };

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);
}

void BezierPatchApp::LoadTextures()
{
    TextureLib& texLib = TextureLib::GetLib();
    texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
}

void BezierPatchApp::BuildCbvSrvUavDescriptorHeap()
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

void BezierPatchApp::BuildRootSignature()
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
        IID_PPV_ARGS(&mRootSignature)));
}

void BezierPatchApp::BuildShadersAndInputLayout()
{
#if defined(DEBUG) || defined(_DEBUG)  
#define COMMA_DEBUG_ARGS ,DXC_ARG_DEBUG, DXC_ARG_SKIP_OPTIMIZATIONS
#else
#define COMMA_DEBUG_ARGS
#endif

    std::vector<LPCWSTR> vsArgs = std::vector<LPCWSTR> { L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> hsArgs = std::vector<LPCWSTR> { L"-E", L"HS", L"-T", L"hs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> dsArgs = std::vector<LPCWSTR> { L"-E", L"DS", L"-T", L"ds_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> psArgs = std::vector<LPCWSTR> { L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> psAlphaTestedArgs = std::vector<LPCWSTR> { L"-E", L"PS", L"-T", L"ps_6_6", L"-D ALPHA_TEST=1" COMMA_DEBUG_ARGS };

    mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", vsArgs);
    mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", psArgs);
    mShaders["alphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", psAlphaTestedArgs);

    mShaders["tessVS"] = d3dUtil::CompileShader(L"Shaders\\BezierTessellation.hlsl", vsArgs);
    mShaders["tessHS"] = d3dUtil::CompileShader(L"Shaders\\BezierTessellation.hlsl", hsArgs);
    mShaders["tessDS"] = d3dUtil::CompileShader(L"Shaders\\BezierTessellation.hlsl", dsArgs);
    mShaders["tessPS"] = d3dUtil::CompileShader(L"Shaders\\BezierTessellation.hlsl", psArgs);

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void BezierPatchApp::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc = d3dUtil::InitDefaultPso(
        mBackBufferFormat, 
        mDepthStencilFormat, 
        mInputLayout,
        mRootSignature.Get(),
        mShaders["standardVS"].Get(),
        mShaders["opaquePS"].Get());

    //
    // PSO for tessellated opaque objects
    // 
    
    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueTessDesc = basePsoDesc;

    opaqueTessDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    opaqueTessDesc.pRootSignature = mRootSignature.Get();
    opaqueTessDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["tessVS"]->GetBufferPointer()),
        mShaders["tessVS"]->GetBufferSize()
    };
    opaqueTessDesc.HS =
    {
        reinterpret_cast<BYTE*>(mShaders["tessHS"]->GetBufferPointer()),
        mShaders["tessHS"]->GetBufferSize()
    };
    opaqueTessDesc.DS =
    {
        reinterpret_cast<BYTE*>(mShaders["tessDS"]->GetBufferPointer()),
        mShaders["tessDS"]->GetBufferSize()
    };
    opaqueTessDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["tessPS"]->GetBufferPointer()),
        mShaders["tessPS"]->GetBufferSize()
    };
    opaqueTessDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    opaqueTessDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueTessDesc, IID_PPV_ARGS(&mPSOs["opaque_tess"])));
}

void BezierPatchApp::BuildFrameResources()
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

void BezierPatchApp::BuildMaterials()
{
    MaterialLib::GetLib().Init(md3dDevice.Get());
}

void BezierPatchApp::AddRenderItem(
    RenderLayer layer, 
    const DirectX::XMFLOAT4X4& world, 
    const XMFLOAT4X4& texTransform, 
    Material* mat, 
    MeshGeometry* geo, 
    SubmeshGeometry& drawArgs,
    D3D_PRIMITIVE_TOPOLOGY primType)
{
    auto ritem = std::make_unique<RenderItem>();
    ritem->World = world;
    ritem->TexTransform = texTransform;
    ritem->Mat = mat;
    ritem->Geo = geo;
    ritem->PrimitiveType = primType;
    ritem->IndexCount = drawArgs.IndexCount;
    ritem->StartIndexLocation = drawArgs.StartIndexLocation;
    ritem->BaseVertexLocation = drawArgs.BaseVertexLocation;

    mRitemLayer[(int)layer].push_back(ritem.get());
    mAllRitems.push_back(std::move(ritem));
}

void BezierPatchApp::BuildRenderItems()
{
    MaterialLib& matLib = MaterialLib::GetLib();

    XMFLOAT4X4 worldTransform = MathHelper::Identity4x4();
    XMFLOAT4X4 texTransform = MathHelper::Identity4x4();
    AddRenderItem(
        RenderLayer::OpaqueTess, 
        worldTransform, 
        texTransform, 
        matLib["whiteMat"], 
        mGeometries["quadpatchGeo"].get(), 
        mGeometries["quadpatchGeo"]->DrawArgs["quadpatch"],
        D3D_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST);

    mPatchRitem = mAllRitems.back().get();
}

void BezierPatchApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
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

std::unique_ptr<MeshGeometry> BezierPatchApp::BuildQuadPatchGeometry()
{
    std::array<XMFLOAT3, 16> vertices =
    {
        // Row 0
        XMFLOAT3(-10.0f, -10.0f, +15.0f),
        XMFLOAT3(-5.0f,  0.0f, +15.0f),
        XMFLOAT3(+5.0f,  0.0f, +15.0f),
        XMFLOAT3(+10.0f, 0.0f, +15.0f),

        // Row 1
        XMFLOAT3(-15.0f, 0.0f, +5.0f),
        XMFLOAT3(-5.0f,  0.0f, +5.0f),
        XMFLOAT3(+5.0f,  20.0f, +5.0f),
        XMFLOAT3(+15.0f, 0.0f, +5.0f),

        // Row 2
        XMFLOAT3(-15.0f, 0.0f, -5.0f),
        XMFLOAT3(-5.0f,  0.0f, -5.0f),
        XMFLOAT3(+5.0f,  0.0f, -5.0f),
        XMFLOAT3(+15.0f, 0.0f, -5.0f),

        // Row 3
        XMFLOAT3(-10.0f, 10.0f, -15.0f),
        XMFLOAT3(-5.0f,  0.0f, -15.0f),
        XMFLOAT3(+5.0f,  0.0f, -15.0f),
        XMFLOAT3(+25.0f, 10.0f, -15.0f)
    };

    std::array<uint16_t, 16> indices =
    {
        0, 1, 2, 3,
        4, 5, 6, 7,
        8, 9, 10, 11,
        12, 13, 14, 15
    };

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(XMFLOAT3);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(uint16_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "quadpatchGeo";

    geo->VertexBufferCPU.resize(vbByteSize);
    CopyMemory(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

    geo->IndexBufferCPU.resize(ibByteSize);
    CopyMemory(geo->IndexBufferCPU.data(), indices.data(), ibByteSize);

    CreateStaticBuffer(md3dDevice.Get(), *mUploadBatch,
                       vertices.data(), vertices.size(), sizeof(XMFLOAT3),
                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &geo->VertexBufferGPU);

    CreateStaticBuffer(md3dDevice.Get(), *mUploadBatch,
                       indices.data(), indices.size(), sizeof(uint16_t),
                       D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);

    geo->VertexByteStride = sizeof(XMFLOAT3);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry quadSubmesh;
    quadSubmesh.IndexCount = (UINT)indices.size();
    quadSubmesh.StartIndexLocation = 0;
    quadSubmesh.BaseVertexLocation = 0;
    quadSubmesh.VertexCount = (UINT)vertices.size();
    quadSubmesh.Bounds = BoundingBox(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(10.0f, 10.0f, 10.0f));

    geo->DrawArgs["quadpatch"] = quadSubmesh;

    return geo;
}