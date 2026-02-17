
#include "WavesCSApp.h"

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
        WavesCSApp theApp(hInstance);
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

WavesCSApp::WavesCSApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{

}

WavesCSApp::~WavesCSApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool WavesCSApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

    

    // We will upload on the direct queue for the book samples, but 
    // copy queue would be better for real game.
    mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

    mWaves = std::make_unique<GpuWaves>(
        md3dDevice.Get(),
        *mUploadBatch,
        256, 256, 0.25f, 0.016f, mWaveSpeed, mWaveDamping);

    LoadTextures();

    std::unique_ptr<MeshGeometry> shapeGeo = d3dUtil::BuildShapeGeometry(md3dDevice.Get(), *mUploadBatch.get());
    if(shapeGeo != nullptr)
    {
        mGeometries[shapeGeo->Name] = std::move(shapeGeo);
    }

    std::unique_ptr<MeshGeometry> landGeo = BuildLandGeometry(
        md3dDevice.Get(), *mUploadBatch.get());
    if(landGeo != nullptr)
    {
        mGeometries[landGeo->Name] = std::move(landGeo);
    }

    std::unique_ptr<MeshGeometry> waveGeo = BuildWaveGeometry(
        md3dDevice.Get(), *mUploadBatch.get());
    if(waveGeo != nullptr)
    {
        mGeometries[waveGeo->Name] = std::move(waveGeo);
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

void WavesCSApp::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
}
 
void WavesCSApp::OnResize()
{
    D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);
}

void WavesCSApp::Update(const GameTimer& gt)
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

void WavesCSApp::Draw(const GameTimer& gt)
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

    auto passCB = mCurrFrameResource->PassCB->Resource();

    UpdateWavesGPU(gt, passCB);

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
    float clearColor[4] = { 0.0f, 0.0f, 0.2f, 0.0f };
    if(mFogEnabled)
    {
        // Use fog color for background
        clearColor[0] = mFogColor.x;
        clearColor[1] = mFogColor.y;
        clearColor[2] = mFogColor.z;
    }
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), clearColor, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

    mCommandList->SetPipelineState(
        mDrawWireframe ? 
        mPSOs["opaque_wireframe"].Get() : 
        mPSOs["opaque"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    mCommandList->SetPipelineState(
        mDrawWireframe ?
        mPSOs["opaque_wireframe"].Get() :
        mPSOs["alphaTested"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::AlphaTested]);

    mCommandList->SetPipelineState(
        mDrawWireframe ?
        mPSOs["opaque_wireframe"].Get() :
        mPSOs["transparent"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Transparent]);

    mCommandList->SetPipelineState(
        mDrawWireframe ?
        mPSOs["waves_wireframe"].Get() :
        mPSOs["waves_transparent"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::GpuWaves]);

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

void WavesCSApp::UpdateImgui(const GameTimer& gt)
{
    D3DApp::UpdateImgui(gt);

    //
    // Define a panel to render GUI elements.
    // 
    ImGui::Begin("Options");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

    ImGui::Checkbox("Wireframe", &mDrawWireframe);
    ImGui::Checkbox("FogEnabled", &mFogEnabled);

    ImGui::SliderFloat("FogStart", &mFogStart, 10.0f, 100);
    ImGui::SliderFloat("FogEnd", &mFogEnd, 20.0f, 200.0f);

    if(mFogStart >= mFogEnd)
        mFogStart = 10.0f;

    ImGui::SliderFloat("WaveScale", &mWaveScale, 0.25f, 4.0f);
    ImGui::SliderFloat("WaveSpeed", &mWaveSpeed, 2.0f, 16.0f);
    ImGui::SliderFloat("WaveDamping", &mWaveDamping, 0.0f, 3.0f);

    mWaves->SetConstants(mWaveSpeed, mWaveDamping);

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

void WavesCSApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        SetCapture(mhMainWnd);
    }
}

void WavesCSApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void WavesCSApp::OnMouseMove(WPARAM btnState, int x, int y)
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

void WavesCSApp::OnKeyboardInput(const GameTimer& gt)
{
}

void WavesCSApp::AnimateMaterials(const GameTimer& gt)
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

void WavesCSApp::UpdateCamera(const GameTimer& gt)
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

void WavesCSApp::UpdatePerObjectCB(const GameTimer& gt)
{
    for(auto& ri : mRitemLayer[(int)RenderLayer::GpuWaves])
    {
        // The current solution displacement map gets ping-ponged every frame, 
        // so we need to set it every frame.
        ri->MiscUint4.x = mWaves->DisplacementMapSrvIndex();
    }

    // Update per object constants once per frame so the data can be shared across different render passes.
    for(auto& ri : mAllRitems)
    {
        XMStoreFloat4x4(&ri->ObjectConstants.gWorld, XMMatrixTranspose(XMLoadFloat4x4(&ri->World)));
        XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, XMMatrixTranspose(XMLoadFloat4x4(&ri->TexTransform)));
        ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;

        ri->ObjectConstants.gMiscUint4 = ri->MiscUint4;
        ri->ObjectConstants.gMiscFloat4 = ri->MiscFloat4;

        // Need to hold handle until we submit work to GPU.
        ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectConstants);
    }
}

void WavesCSApp::UpdateMaterialBuffer(const GameTimer& gt)
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

void WavesCSApp::UpdateMainPassCB(const GameTimer& gt)
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

    mMainPassCB.gFogColor = mFogColor;
    mMainPassCB.gFogStart = mFogStart;
    mMainPassCB.gFogRange = mFogEnd - mFogStart;
    mMainPassCB.gFogEnabled = mFogEnabled;

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

void WavesCSApp::UpdateWavesGPU(const GameTimer& gt, ID3D12Resource* passCB)
{
    // Every quarter second, generate a random wave.
    static float t_base = 0.0f;
    if((mTimer.TotalTime() - t_base) >= 0.25f)
    {
        t_base += 0.25f;

        int i = MathHelper::Rand(4, mWaves->RowCount() - 5);
        int j = MathHelper::Rand(4, mWaves->ColumnCount() - 5);

        float r = MathHelper::RandF(1.0f, 2.0f);

        mWaves->Disturb(mCommandList.Get(), mComputeRootSignature.Get(), passCB, mPSOs["wavesDisturb"].Get(), i, j, r);
    }

    // Update the wave simulation.
    mWaves->Update(gt, mCommandList.Get(), mComputeRootSignature.Get(), passCB, mPSOs["wavesUpdate"].Get());
}

void WavesCSApp::LoadTextures()
{
    TextureLib& texLib = TextureLib::GetLib();
    texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
}

void WavesCSApp::BuildCbvSrvUavDescriptorHeap()
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

    mWaves->BuildDescriptors();
}

void WavesCSApp::BuildRootSignature()
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

    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER computeRootParameters[COMPUTE_ROOT_ARG_COUNT];

    // Perfomance TIP: Order from most frequent to least frequent.
    computeRootParameters[COMPUTE_ROOT_ARG_DISPATCH_CBV].InitAsConstantBufferView(0);
    computeRootParameters[COMPUTE_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);
    computeRootParameters[COMPUTE_ROOT_ARG_PASS_EXTRA_CBV].InitAsConstantBufferView(2);

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

void WavesCSApp::BuildShadersAndInputLayout()
{
#if defined(DEBUG) || defined(_DEBUG)  
#define COMMA_DEBUG_ARGS ,DXC_ARG_DEBUG, DXC_ARG_SKIP_OPTIMIZATIONS
#else
#define COMMA_DEBUG_ARGS
#endif

    std::vector<LPCWSTR> vsArgs = std::vector<LPCWSTR> { L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> vsWavesArgs = std::vector<LPCWSTR> { L"-E", L"VS", L"-T", L"vs_6_6", L"-D WAVES_VS=1" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> psArgs = std::vector<LPCWSTR> { L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> psAlphaTestedArgs = std::vector<LPCWSTR> { L"-E", L"PS", L"-T", L"ps_6_6", L"-D ALPHA_TEST=1" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> csUpdateWavesArgs = std::vector<LPCWSTR> { L"-E", L"UpdateWavesCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> csDisturbWavesArgs = std::vector<LPCWSTR> { L"-E", L"DisturbWavesCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };


    mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", vsArgs);
    mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", psArgs);
    mShaders["alphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", psAlphaTestedArgs);

    mShaders["wavesVS"] = d3dUtil::CompileShader(L"Shaders\\BasicBlend.hlsl", vsWavesArgs);
    mShaders["wavesUpdateCS"] = d3dUtil::CompileShader(L"Shaders\\WaveSim.hlsl", csUpdateWavesArgs);
    mShaders["wavesDisturbCS"] = d3dUtil::CompileShader(L"Shaders\\WaveSim.hlsl", csDisturbWavesArgs);

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void WavesCSApp::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc = d3dUtil::InitDefaultPso(
        mBackBufferFormat, 
        mDepthStencilFormat, 
        mInputLayout,
        mRootSignature.Get(),
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
    // PSO for transparent objects
    //

    D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc = basePsoDesc;

    D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc;
    transparencyBlendDesc.BlendEnable = true;
    transparencyBlendDesc.LogicOpEnable = false;
    transparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    transparentPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc,
                  IID_PPV_ARGS(&mPSOs["transparent"])));

    //
    // PSO for alpha tested objects
    //

    D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestedPsoDesc = basePsoDesc;
    alphaTestedPsoDesc.PS = d3dUtil::ByteCodeFromBlob(mShaders["alphaTestedPS"].Get());
    alphaTestedPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&alphaTestedPsoDesc, 
                  IID_PPV_ARGS(&mPSOs["alphaTested"])));

    //
    // PSO for drawing waves
    //
    D3D12_GRAPHICS_PIPELINE_STATE_DESC wavesRenderPSO = transparentPsoDesc;
    wavesRenderPSO.VS = d3dUtil::ByteCodeFromBlob(mShaders["wavesVS"].Get());
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&wavesRenderPSO, IID_PPV_ARGS(&mPSOs["waves_transparent"])));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC wavesRenderWireframePSO = basePsoDesc;
    wavesRenderWireframePSO.VS = d3dUtil::ByteCodeFromBlob(mShaders["wavesVS"].Get());
    wavesRenderWireframePSO.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&wavesRenderWireframePSO, IID_PPV_ARGS(&mPSOs["waves_wireframe"])));



    //
    // PSO for disturbing waves
    //
    D3D12_COMPUTE_PIPELINE_STATE_DESC wavesDisturbPSO = {};
    wavesDisturbPSO.pRootSignature = mComputeRootSignature.Get();
    wavesDisturbPSO.CS = d3dUtil::ByteCodeFromBlob(mShaders["wavesDisturbCS"].Get());
    wavesDisturbPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateComputePipelineState(&wavesDisturbPSO, IID_PPV_ARGS(&mPSOs["wavesDisturb"])));

    //
    // PSO for updating waves
    //
    D3D12_COMPUTE_PIPELINE_STATE_DESC wavesUpdatePSO = {};
    wavesUpdatePSO.pRootSignature = mComputeRootSignature.Get();
    wavesUpdatePSO.CS = d3dUtil::ByteCodeFromBlob(mShaders["wavesUpdateCS"].Get());
    wavesUpdatePSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateComputePipelineState(&wavesUpdatePSO, IID_PPV_ARGS(&mPSOs["wavesUpdate"])));
}

void WavesCSApp::BuildFrameResources()
{
    MaterialLib& matLib = MaterialLib::GetLib();

    constexpr UINT passCount = 1;
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(
            std::make_unique<FrameResource>(md3dDevice.Get(),
            passCount, matLib.GetMaterialCount(), mWaves->VertexCount()));
    }
}

void WavesCSApp::BuildMaterials()
{
    MaterialLib::GetLib().Init(md3dDevice.Get());
}

void WavesCSApp::AddRenderItem(RenderLayer layer, const DirectX::XMFLOAT4X4& world, const XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs)
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

void WavesCSApp::AddWaveRenderItem(
    RenderLayer layer,
    const DirectX::XMFLOAT4X4& world,
    const DirectX::XMFLOAT4X4& texTransform,
    uint32_t wavesGridWidth,
    uint32_t wavesGridDepth,
    float wavesGridSpatialStep,
    Material* mat,
    MeshGeometry* geo,
    SubmeshGeometry& drawArgs)
{
    AddRenderItem(layer,
                  world,
                  texTransform,
                  mat,
                  geo,
                  drawArgs);

    mAllRitems.back()->MiscUint4.y = wavesGridWidth;
    mAllRitems.back()->MiscUint4.z = wavesGridDepth;
    mAllRitems.back()->MiscFloat4.x = wavesGridSpatialStep;
}

void WavesCSApp::BuildRenderItems()
{
    MaterialLib& matLib = MaterialLib::GetLib();

    XMFLOAT4X4 worldTransform = MathHelper::Identity4x4();
    XMFLOAT4X4 texTransform = MathHelper::Identity4x4();

    worldTransform = MathHelper::Identity4x4();
    XMStoreFloat4x4(&texTransform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
    AddWaveRenderItem(
        RenderLayer::GpuWaves,
        worldTransform, 
        texTransform,
        mWaves->ColumnCount(),
        mWaves->RowCount(),
        mWaves->SpatialStep(),
        matLib["water"], 
        mGeometries["waterGeo"].get(), 
        mGeometries["waterGeo"]->DrawArgs["grid"]);

    worldTransform = MathHelper::Identity4x4();
    XMStoreFloat4x4(&texTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
    AddRenderItem(RenderLayer::Opaque, 
                  worldTransform, 
                  texTransform, 
                  matLib["grass"], 
                  mGeometries["landGeo"].get(),
                  mGeometries["landGeo"]->DrawArgs["grid"]);

    XMMATRIX S = XMMatrixScaling(8.0f, 8.0f, 8.0f);
    XMMATRIX T = XMMatrixTranslation(3.0f, 2.0f, -9.0f);
    XMStoreFloat4x4(&worldTransform, S * T);
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::AlphaTested, worldTransform, texTransform, matLib["fence"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["box"]);
}

void WavesCSApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
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

std::unique_ptr<MeshGeometry> WavesCSApp::BuildLandGeometry(
    ID3D12Device* device,
    ResourceUploadBatch& uploadBatch)
{
    MeshGen meshGen;
    MeshGenData grid = meshGen.CreateGrid(160.0f, 160.0f, 50, 50);

    //
    // Extract the vertex elements we are interested and apply the height function to
    // each vertex.  In addition, color the vertices based on their height so we have
    // sandy looking beaches, grassy low hills, and snow mountain peaks.
    //

    std::vector<ModelVertex> vertices(grid.Vertices.size());
    for(size_t i = 0; i < grid.Vertices.size(); ++i)
    {
        auto& p = grid.Vertices[i].Position;
        vertices[i].Pos = p;
        vertices[i].Pos.y = GetHillsHeight(p.x, p.z);

        vertices[i].Normal = GetHillsNormal(p.x, p.z);

        vertices[i].TexC = grid.Vertices[i].TexC;

        // Not used in this demo.
        vertices[i].TangentU = XMFLOAT3(0.0f, 0.0f, 0.0f);
    }

    const uint32_t indexCount = (UINT)grid.Indices32.size();
    const UINT indexElementByteSize = sizeof(uint16_t);
    const UINT vbByteSize = (UINT)vertices.size() * sizeof(ModelVertex);
    const UINT ibByteSize = indexCount * indexElementByteSize;

    std::vector<std::uint16_t> indices = grid.GetIndices16();

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "landGeo";

    geo->VertexBufferCPU.resize(vbByteSize);
    CopyMemory(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

    geo->IndexBufferCPU.resize(ibByteSize);
    CopyMemory(geo->IndexBufferCPU.data(), indices.data(), ibByteSize);

    CreateStaticBuffer(
        device, uploadBatch,
        vertices.data(), vertices.size(), sizeof(ModelVertex),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        &geo->VertexBufferGPU);

    CreateStaticBuffer(
        device, uploadBatch,
        indices.data(), indexCount, indexElementByteSize,
        D3D12_RESOURCE_STATE_INDEX_BUFFER,
        &geo->IndexBufferGPU);

    geo->VertexByteStride = sizeof(ModelVertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    submesh.VertexCount = (UINT)vertices.size();
    geo->DrawArgs["grid"] = submesh;

    return geo;
}

std::unique_ptr<MeshGeometry> WavesCSApp::BuildWaveGeometry(
    ID3D12Device* device,
    ResourceUploadBatch& uploadBatch)
{
    int m = mWaves->RowCount();
    int n = mWaves->ColumnCount();

    const float waterWorldSize = 128.0f;

    MeshGen meshGen;
    MeshGenData grid = meshGen.CreateGrid(waterWorldSize, waterWorldSize, m, n);


    // Extract the vertex elements we are interested into our vertex buffer. 
    std::vector<ModelVertex> vertices(grid.Vertices.size());
    for(size_t i = 0; i < grid.Vertices.size(); ++i)
    {
        vertices[i].Pos = grid.Vertices[i].Position;
        vertices[i].Normal = grid.Vertices[i].Normal;
        vertices[i].TexC = grid.Vertices[i].TexC;
        vertices[i].TangentU = grid.Vertices[i].TangentU;
    }

    const uint32_t indexCount = (UINT)grid.Indices32.size();

    const UINT indexElementByteSize = sizeof(uint32_t);
    const UINT vbByteSize = (UINT)vertices.size() * sizeof(ModelVertex);
    const UINT ibByteSize = indexCount * indexElementByteSize;

    const byte* indexData = reinterpret_cast<byte*>(grid.Indices32.data());

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "waterGeo";

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
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = indexCount;
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    submesh.VertexCount = static_cast<UINT>(vertices.size());
    submesh.Bounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
    submesh.Bounds.Extents = XMFLOAT3(waterWorldSize, waterWorldSize, 2.0f);

    geo->DrawArgs["grid"] = submesh;

    return geo;
}

float WavesCSApp::GetHillsHeight(float x, float z)const
{
    return 0.3f*(z*sinf(0.1f*x) + x*cosf(0.1f*z));
}

DirectX::XMFLOAT3 WavesCSApp::GetHillsNormal(float x, float z)const
{
    // n = (-df/dx, 1, -df/dz)
    XMFLOAT3 n(
        -0.03f*z*cosf(0.1f*x) - 0.3f*cosf(0.1f*z),
        1.0f,
        -0.3f*sinf(0.1f*x) + 0.03f*x*sinf(0.1f*z));

    XMVECTOR unitNormal = XMVector3Normalize(XMLoadFloat3(&n));
    XMStoreFloat3(&n, unitNormal);

    return n;
}