
#include "StencilingApp.h"

// Use keys 'A', 'D', 'W', 'S' to move the skull.

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
        StencilingApp theApp(hInstance);
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

StencilingApp::StencilingApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{

}

StencilingApp::~StencilingApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool StencilingApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

    // We will upload on the direct queue for the book samples, but 
    // copy queue would be better for real game.
    mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

    LoadTextures();

    std::unique_ptr<MeshGeometry> shapeGeo = BuildRoomGeometry(md3dDevice.Get(), *mUploadBatch.get());
    if(shapeGeo != nullptr)
    {
        mGeometries[shapeGeo->Name] = std::move(shapeGeo);
    }

    std::unique_ptr<MeshGeometry> skullGeo = d3dUtil::BuildSkullGeometry(md3dDevice.Get(), *mUploadBatch.get());
    if(skullGeo != nullptr)
    {
        mGeometries[skullGeo->Name] = std::move(skullGeo);
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

void StencilingApp::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
}
 
void StencilingApp::OnResize()
{
    D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);
}

void StencilingApp::Update(const GameTimer& gt)
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
    UpdateReflectedPassCB(gt);
}

void StencilingApp::Draw(const GameTimer& gt)
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
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::Black, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

    // Draw opaque items--floors, walls, skull.
    mCommandList->SetPipelineState(
        mDrawWireframe ? 
        mPSOs["opaque_wireframe"].Get() : 
        mPSOs["opaque"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    // Mark the visible mirror pixels in the stencil buffer with the value 1
    mCommandList->OMSetStencilRef(1);
    mCommandList->SetPipelineState(mPSOs["markStencilMirrors"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Mirrors]);

    // Draw the reflection into the mirror only (only for pixels where the stencil buffer is 1).
    // Note that we must supply a different per-pass constant buffer--one with the lights reflected.
    UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerPassCB));
    mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress() + 1 * passCBByteSize);
    mCommandList->SetPipelineState(mPSOs["drawStencilReflections"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Reflected]);

    // Restore main pass constants and stencil ref.
    mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());
    mCommandList->OMSetStencilRef(0);

    // Draw mirror with transparency so reflection blends through.
    mCommandList->SetPipelineState(mPSOs["transparent"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Transparent]);

    // Draw shadows
    mCommandList->SetPipelineState(mPSOs["shadow"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Shadow]);

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

void StencilingApp::UpdateImgui(const GameTimer& gt)
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

void StencilingApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        SetCapture(mhMainWnd);
    }
}

void StencilingApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void StencilingApp::OnMouseMove(WPARAM btnState, int x, int y)
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

void StencilingApp::OnKeyboardInput(const GameTimer& gt)
{
    //
    // Allow user to move skull.
    //

    const float dt = gt.DeltaTime();

    if(GetAsyncKeyState('A') & 0x8000)
        mSkullTranslation.x -= 1.0f*dt;

    if(GetAsyncKeyState('D') & 0x8000)
        mSkullTranslation.x += 1.0f*dt;

    if(GetAsyncKeyState('W') & 0x8000)
        mSkullTranslation.y += 1.0f*dt;

    if(GetAsyncKeyState('S') & 0x8000)
        mSkullTranslation.y -= 1.0f*dt;

    // Don't let user move below ground plane.
    mSkullTranslation.y = MathHelper::Max(mSkullTranslation.y, 0.0f);

    // Update the new world matrix.
    XMMATRIX skullRotate = XMMatrixRotationY(0.5f*MathHelper::Pi);
    XMMATRIX skullScale = XMMatrixScaling(0.45f, 0.45f, 0.45f);
    XMMATRIX skullOffset = XMMatrixTranslation(mSkullTranslation.x, mSkullTranslation.y, mSkullTranslation.z);
    XMMATRIX skullWorld = skullRotate*skullScale*skullOffset;
    XMStoreFloat4x4(&mSkullRitem->World, skullWorld);

    // Update reflection world matrix.
    XMVECTOR mirrorPlane = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); // xy plane
    XMMATRIX R = XMMatrixReflect(mirrorPlane);
    XMStoreFloat4x4(&mReflectedSkullRitem->World, skullWorld * R);

    // Update shadow world matrix.
    XMVECTOR shadowPlane = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f); // xz plane
    XMVECTOR toMainLight = -XMLoadFloat3(&mMainPassCB.gLights[0].Direction);
    XMMATRIX S = XMMatrixShadow(shadowPlane, toMainLight);
    XMMATRIX shadowOffsetY = XMMatrixTranslation(0.0f, 0.001f, 0.0f);
    XMStoreFloat4x4(&mShadowedSkullRitem->World, skullWorld * S * shadowOffsetY);
}

void StencilingApp::AnimateMaterials(const GameTimer& gt)
{
}

void StencilingApp::UpdateCamera(const GameTimer& gt)
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

void StencilingApp::UpdatePerObjectCB(const GameTimer& gt)
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

void StencilingApp::UpdateMaterialBuffer(const GameTimer& gt)
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

void StencilingApp::UpdateMainPassCB(const GameTimer& gt)
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

void StencilingApp::UpdateReflectedPassCB(const GameTimer& gt)
{
    mReflectedPassCB = mMainPassCB;

    XMVECTOR mirrorPlane = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); // xy plane
    XMMATRIX R = XMMatrixReflect(mirrorPlane);

    // Reflect the lighting.
    for(int i = 0; i < 3; ++i)
    {
        XMVECTOR lightDir = XMLoadFloat3(&mMainPassCB.gLights[i].Direction);
        XMVECTOR reflectedLightDir = XMVector3TransformNormal(lightDir, R);
        XMStoreFloat3(&mReflectedPassCB.gLights[i].Direction, reflectedLightDir);
    }

    // Reflected pass stored in index 1
    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(1, mReflectedPassCB);
}

void StencilingApp::LoadTextures()
{
    TextureLib& texLib = TextureLib::GetLib();
    texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
}

void StencilingApp::BuildCbvSrvUavDescriptorHeap()
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

void StencilingApp::BuildRootSignature()
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

void StencilingApp::BuildShadersAndInputLayout()
{
#if defined(DEBUG) || defined(_DEBUG)  
#define COMMA_DEBUG_ARGS ,DXC_ARG_DEBUG, DXC_ARG_SKIP_OPTIMIZATIONS
#else
#define COMMA_DEBUG_ARGS
#endif

    std::vector<LPCWSTR> vsArgs = std::vector<LPCWSTR> { L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> psArgs = std::vector<LPCWSTR> { L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

    mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicTex.hlsl", vsArgs);
    mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicTex.hlsl", psArgs);

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void StencilingApp::BuildPSOs()
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
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&mPSOs["transparent"])));

    //
    // PSO for marking stencil mirrors.
    //

    CD3DX12_BLEND_DESC mirrorBlendState(D3D12_DEFAULT);
    mirrorBlendState.RenderTarget[0].RenderTargetWriteMask = 0;

    D3D12_DEPTH_STENCIL_DESC mirrorDSS;
    mirrorDSS.DepthEnable = true;
    mirrorDSS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    mirrorDSS.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    mirrorDSS.StencilEnable = true;
    mirrorDSS.StencilReadMask = 0xff;
    mirrorDSS.StencilWriteMask = 0xff;

    mirrorDSS.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    mirrorDSS.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    mirrorDSS.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    mirrorDSS.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    // We are not rendering backfacing polygons, so these settings do not matter.
    mirrorDSS.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    mirrorDSS.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    mirrorDSS.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    mirrorDSS.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC markMirrorsPsoDesc = basePsoDesc;
    markMirrorsPsoDesc.BlendState = mirrorBlendState;
    markMirrorsPsoDesc.DepthStencilState = mirrorDSS;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &markMirrorsPsoDesc, 
        IID_PPV_ARGS(&mPSOs["markStencilMirrors"])));

    //
    // PSO for stencil reflections.
    //

    D3D12_DEPTH_STENCIL_DESC reflectionsDSS;
    reflectionsDSS.DepthEnable = true;
    reflectionsDSS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    reflectionsDSS.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    reflectionsDSS.StencilEnable = true;
    reflectionsDSS.StencilReadMask = 0xff;
    reflectionsDSS.StencilWriteMask = 0xff;

    reflectionsDSS.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    reflectionsDSS.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    reflectionsDSS.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    reflectionsDSS.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

    // We are not rendering backfacing polygons, so these settings do not matter.
    reflectionsDSS.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    reflectionsDSS.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    reflectionsDSS.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    reflectionsDSS.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC drawReflectionsPsoDesc = basePsoDesc;
    drawReflectionsPsoDesc.DepthStencilState = reflectionsDSS;
    drawReflectionsPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    drawReflectionsPsoDesc.RasterizerState.FrontCounterClockwise = true;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&drawReflectionsPsoDesc, IID_PPV_ARGS(&mPSOs["drawStencilReflections"])));

    //
    // PSO for shadow objects
    //

    // We are going to draw shadows with transparency, so base it off the transparency description.
    D3D12_DEPTH_STENCIL_DESC shadowDSS;
    shadowDSS.DepthEnable = true;
    shadowDSS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    shadowDSS.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    shadowDSS.StencilEnable = true;
    shadowDSS.StencilReadMask = 0xff;
    shadowDSS.StencilWriteMask = 0xff;

    shadowDSS.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    shadowDSS.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    shadowDSS.FrontFace.StencilPassOp = D3D12_STENCIL_OP_INCR;
    shadowDSS.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

    // We are not rendering backfacing polygons, so these settings do not matter.
    shadowDSS.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    shadowDSS.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    shadowDSS.BackFace.StencilPassOp = D3D12_STENCIL_OP_INCR;
    shadowDSS.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPsoDesc = transparentPsoDesc;
    shadowPsoDesc.DepthStencilState = shadowDSS;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&shadowPsoDesc, IID_PPV_ARGS(&mPSOs["shadow"])));
}

void StencilingApp::BuildFrameResources()
{
    MaterialLib& matLib = MaterialLib::GetLib();

    constexpr UINT passCount = 2;
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(
            std::make_unique<FrameResource>(md3dDevice.Get(),
            passCount, matLib.GetMaterialCount()));
    }
}

void StencilingApp::BuildMaterials()
{
    MaterialLib& matLib = MaterialLib::GetLib();
    matLib.Init(md3dDevice.Get());

    TextureLib& texLib = TextureLib::GetLib();

    // These are specific to the stenciling demo, so we are not going to put them in the matLib.
    matLib.AddMaterial("checkertile",
                texLib["checkboardMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                XMFLOAT3(0.07f, 0.07f, 0.07f), 0.3f);

    matLib.AddMaterial("icemirror",
                texLib["iceMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 0.6f),
                XMFLOAT3(0.1f, 0.1f, 0.1f), 0.5f);

    matLib.AddMaterial("shadowMat",
                texLib["defaultDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(0.0f, 0.0f, 0.0f, 0.5f),
                XMFLOAT3(0.01f, 0.01f, 0.01f), 0.0f);
}

RenderItem* StencilingApp::AddRenderItem(RenderLayer layer, const DirectX::XMFLOAT4X4& world, const XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs)
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

    return mAllRitems.back().get();
}

void StencilingApp::BuildRenderItems()
{
    MaterialLib& matLib = MaterialLib::GetLib();

    XMFLOAT4X4 worldTransform = MathHelper::Identity4x4();
    XMFLOAT4X4 texTransform = MathHelper::Identity4x4();

    AddRenderItem(
        RenderLayer::Opaque, 
        worldTransform, 
        texTransform, 
        matLib["checkertile"], 
        mGeometries["roomGeo"].get(), 
        mGeometries["roomGeo"]->DrawArgs["floor"]);

    AddRenderItem(
        RenderLayer::Opaque,
        worldTransform,
        texTransform,
        matLib["bricks0"],
        mGeometries["roomGeo"].get(),
        mGeometries["roomGeo"]->DrawArgs["wall"]);

    mSkullRitem = AddRenderItem(
        RenderLayer::Opaque,
        worldTransform,
        texTransform,
        matLib["skullMat"],
        mGeometries["skullGeo"].get(),
        mGeometries["skullGeo"]->DrawArgs["skull"]);

    // Reflected skull will have different world matrix, so it needs to be its own render item.
    mReflectedSkullRitem = AddRenderItem(
        RenderLayer::Reflected,
        worldTransform,
        texTransform,
        matLib["skullMat"],
        mGeometries["skullGeo"].get(),
        mGeometries["skullGeo"]->DrawArgs["skull"]);

    // Shadowed skull will have different world matrix, so it needs to be its own render item.
    mShadowedSkullRitem = AddRenderItem(
        RenderLayer::Shadow,
        worldTransform,
        texTransform,
        matLib["shadowMat"],
        mGeometries["skullGeo"].get(),
        mGeometries["skullGeo"]->DrawArgs["skull"]);

    AddRenderItem(
        RenderLayer::Mirrors,
        worldTransform,
        texTransform,
        matLib["icemirror"],
        mGeometries["roomGeo"].get(),
        mGeometries["roomGeo"]->DrawArgs["mirror"]);

    AddRenderItem(
        RenderLayer::Transparent,
        worldTransform,
        texTransform,
        matLib["icemirror"],
        mGeometries["roomGeo"].get(),
        mGeometries["roomGeo"]->DrawArgs["mirror"]);
}

void StencilingApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
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

std::unique_ptr<MeshGeometry> StencilingApp::BuildRoomGeometry(
    ID3D12Device* device,
    ResourceUploadBatch& uploadBatch)
{
    // Create and specify geometry.  For this sample we draw a floor
    // and a wall with a mirror on it.  We put the floor, wall, and
    // mirror geometry in one vertex buffer.
    //
    //   |--------------|
    //   |              |
    //   |----|----|----|
    //   |Wall|Mirr|Wall|
    //   |    | or |    |
    //   /--------------/
    //  /   Floor      /
    // /--------------/

    std::array<ModelVertex, 20> vertices =
    {
        // Floor: Observe we tile texture coordinates.
        ModelVertex(-3.5f, 0.0f, -10.0f, 0.0f, 1.0f, 0.0f, 0.0f, 4.0f), // 0 
        ModelVertex(-3.5f, 0.0f,   0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f),
        ModelVertex(7.5f, 0.0f,   0.0f, 0.0f, 1.0f, 0.0f, 4.0f, 0.0f),
        ModelVertex(7.5f, 0.0f, -10.0f, 0.0f, 1.0f, 0.0f, 4.0f, 4.0f),

        // Wall: Observe we tile texture coordinates, and that we
        // leave a gap in the middle for the mirror.
        ModelVertex(-3.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 2.0f), // 4
        ModelVertex(-3.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f),
        ModelVertex(-2.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.5f, 0.0f),
        ModelVertex(-2.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.5f, 2.0f),

        ModelVertex(2.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 2.0f), // 8 
        ModelVertex(2.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f),
        ModelVertex(7.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 2.0f, 0.0f),
        ModelVertex(7.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 2.0f, 2.0f),

        ModelVertex(-3.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f), // 12
        ModelVertex(-3.5f, 6.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f),
        ModelVertex(7.5f, 6.0f, 0.0f, 0.0f, 0.0f, -1.0f, 6.0f, 0.0f),
        ModelVertex(7.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 6.0f, 1.0f),

        // Mirror
        ModelVertex(-2.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f), // 16
        ModelVertex(-2.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f),
        ModelVertex(2.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f),
        ModelVertex(2.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f)
    };

    std::array<uint16_t, 30> indices =
    {
        // Floor
        0, 1, 2,
        0, 2, 3,

        // Walls
        4, 5, 6,
        4, 6, 7,

        8, 9, 10,
        8, 10, 11,

        12, 13, 14,
        12, 14, 15,

        // Mirror
        16, 17, 18,
        16, 18, 19
    };

    SubmeshGeometry floorSubmesh;
    floorSubmesh.IndexCount = 6;
    floorSubmesh.StartIndexLocation = 0;
    floorSubmesh.BaseVertexLocation = 0;

    SubmeshGeometry wallSubmesh;
    wallSubmesh.IndexCount = 18;
    wallSubmesh.StartIndexLocation = 6;
    wallSubmesh.BaseVertexLocation = 0;

    SubmeshGeometry mirrorSubmesh;
    mirrorSubmesh.IndexCount = 6;
    mirrorSubmesh.StartIndexLocation = 24;
    mirrorSubmesh.BaseVertexLocation = 0;

    const UINT indexCount = (UINT)indices.size();
    const UINT indexElementByteSize = sizeof(uint16_t);
    const UINT vbByteSize = (UINT)vertices.size() * sizeof(ModelVertex);
    const UINT ibByteSize = indexCount * indexElementByteSize;

    const byte* indexData = reinterpret_cast<byte*>(indices.data());

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "roomGeo";

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

    geo->DrawArgs["floor"] = floorSubmesh;
    geo->DrawArgs["wall"] = wallSubmesh;
    geo->DrawArgs["mirror"] = mirrorSubmesh;

    return geo;
}