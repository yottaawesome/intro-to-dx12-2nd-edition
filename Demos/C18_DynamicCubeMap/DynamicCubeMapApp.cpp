
#include "DynamicCubeMapApp.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

constexpr UINT CubeMapSize = 512;

constexpr int OffsetToCubeFace0 = 1;

//
// Define named offsets into descriptor heaps for readability.
//

enum RtvOffsets
{
    // Start after swapchain buffers.
    RTV_CUBE_FACE0 = D3DApp::SwapChainBufferCount,
    RTV_CUBE_FACE1,
    RTV_CUBE_FACE2,
    RTV_CUBE_FACE3,
    RTV_CUBE_FACE4,
    RTV_CUBE_FACE5,
    RTV_COUNT
};

enum DsvOffsets
{
    DSV_MAINVIEW = 0,
    DSV_DYNAMIC_CUBEMAP,
    DSV_COUNT
};

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
        DynamicCubeMap theApp(hInstance);
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

DynamicCubeMap::DynamicCubeMap(HINSTANCE hInstance)
    : D3DApp(hInstance)
{

}

DynamicCubeMap::~DynamicCubeMap()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool DynamicCubeMap::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

	mCamera.SetPosition(0.0f, 2.0f, -15.0f);

    BuildCubeFaceCamera(0.0f, 2.0f, 0.0f);

    mDynamicCubeMap = std::make_unique<CubeRenderTarget>(md3dDevice.Get(),
        CubeMapSize, CubeMapSize, DXGI_FORMAT_R8G8B8A8_UNORM);
 
    // Create the singleton.
    GraphicsMemory::Get(md3dDevice.Get());


    // We will upload on the direct queue for the book samples, but 
    // copy queue would be better for real game.
    mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

    // Do init work that requires mUploadBatch...
	LoadTextures();
    LoadGeometry();

    // Kick off upload work asyncronously.
    std::future<void> result = mUploadBatch->End(mCommandQueue.Get());

    // Other init work.
    BuildRootSignature();
	BuildCbvSrvUavDescriptorHeap();
    BuildCubeDepthStencil();
    BuildShadersAndInputLayout();
	BuildMaterials();
    BuildRenderItems();
    BuildFrameResources();
    BuildPSOs();

    // Block until the upload work is complete.
    result.wait();

    return true;
}

void DynamicCubeMap::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_COUNT);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_COUNT);
}
 
void DynamicCubeMap::OnResize()
{
    D3DApp::OnResize();

	mCamera.SetLens(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
}

void DynamicCubeMap::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);

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
    // Animate the lights (and hence shadows).
    //

    mLightRotationAngle += 0.1f*gt.DeltaTime();

    XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
    for(int i = 0; i < 3; ++i)
    {
        XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
        lightDir = XMVector3TransformNormal(lightDir, R);
        XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
    }

    //
    // Animate the skull around the center sphere.
    //

    XMMATRIX skullScale = XMMatrixScaling(0.2f, 0.2f, 0.2f);
    XMMATRIX skullOffset = XMMatrixTranslation(3.0f, 2.0f, 0.0f);
    XMMATRIX skullLocalRotate = XMMatrixRotationY(2.0f*gt.TotalTime());
    XMMATRIX skullGlobalRotate = XMMatrixRotationY(0.5f*gt.TotalTime());
    XMStoreFloat4x4(&mSkullRitem->World, skullScale*skullLocalRotate*skullOffset*skullGlobalRotate);

	AnimateMaterials(gt);
    constexpr bool cubeMapPass = true;
    UpdatePerObjectCB(gt, cubeMapPass);
	UpdateMaterialBuffer(gt);
	UpdateMainPassCB(gt);
}

void DynamicCubeMap::Draw(const GameTimer& gt)
{
    PsoLib& psoLib = PsoLib::GetLib();
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    SamplerHeap& samHeap = SamplerHeap::Get();

    UpdateImgui(gt);

    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(cmdListAlloc->Reset());

    DrawSceneToCubeMap();

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), psoLib["opaque"]));

    ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavHeap.GetD3dHeap(), samHeap.GetD3dHeap() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    // Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
    // set as a root descriptor.
    auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
    mCommandList->SetGraphicsRootShaderResourceView(GFX_ROOT_ARG_MATERIAL_SRV, matBuffer->GetGPUVirtualAddress());


    constexpr bool cubeMapPass = false;
    UpdatePerObjectCB(gt, cubeMapPass);

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

    mCommandList->SetPipelineState(mDrawWireframe ? psoLib["opaque_wireframe"] : psoLib["opaque"]);
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    mCommandList->SetPipelineState(psoLib["debug"]);
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Debug]);

	mCommandList->SetPipelineState(psoLib["sky"]);
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);

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

void DynamicCubeMap::UpdateImgui(const GameTimer& gt)
{
    D3DApp::UpdateImgui(gt);

    //
    // Define a panel to render GUI elements.
    // 
    ImGui::Begin("Options");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

    ImGui::Checkbox("Wireframe", &mDrawWireframe);
    ImGui::Checkbox("NormalMaps", &mNormalMapsEnabled);
    ImGui::Checkbox("Reflections", &mReflectionsEnabled);

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

void DynamicCubeMap::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        SetCapture(mhMainWnd);
    }
}

void DynamicCubeMap::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void DynamicCubeMap::OnMouseMove(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        if ((btnState & MK_LBUTTON) != 0)
        {
            // Make each pixel correspond to a quarter of a degree.
            float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
            float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

            mCamera.Pitch(dy);
            mCamera.RotateY(dx);
        }

        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }
}
 
void DynamicCubeMap::OnKeyboardInput(const GameTimer& gt)
{
	const float dt = gt.DeltaTime();

	if(GetAsyncKeyState('W') & 0x8000)
		mCamera.Walk(10.0f*dt);

	if(GetAsyncKeyState('S') & 0x8000)
		mCamera.Walk(-10.0f*dt);

	if(GetAsyncKeyState('A') & 0x8000)
		mCamera.Strafe(-10.0f*dt);

	if(GetAsyncKeyState('D') & 0x8000)
		mCamera.Strafe(10.0f*dt);

	mCamera.UpdateViewMatrix();
}
 
void DynamicCubeMap::AnimateMaterials(const GameTimer& gt)
{
	
}

void DynamicCubeMap::UpdatePerObjectCB(const GameTimer& gt, bool cubeMapPass)
{
    // Update per object constants once per frame so the data can be shared across different render passes.
    for (auto& ri : mAllRitems)
    {
        XMStoreFloat4x4(&ri->ObjectConstants.gWorld, XMMatrixTranspose(XMLoadFloat4x4(&ri->World)));
        XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, XMMatrixTranspose(XMLoadFloat4x4(&ri->TexTransform)));
        ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;
        ri->ObjectConstants.gCubeMapIndex = cubeMapPass ? mSkyBindlessIndex : mDynamicCubeMap->BindlessIndex();
        ri->ObjectConstants.gMiscUint4 = XMUINT4(0, 0, 0, 0);
        ri->ObjectConstants.gMiscFloat4 = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);

        // Need to hold handle until we submit work to GPU.
        ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectConstants);
    }
}

void DynamicCubeMap::UpdateMaterialBuffer(const GameTimer& gt)
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

void DynamicCubeMap::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX proj = mCamera.GetProj();

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
	mMainPassCB.gEyePosW = mCamera.GetPosition3f();
	mMainPassCB.gRenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.gInvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.gNearZ = 1.0f;
	mMainPassCB.gFarZ = 1000.0f;
	mMainPassCB.gTotalTime = gt.TotalTime();
	mMainPassCB.gDeltaTime = gt.DeltaTime();
	mMainPassCB.gAmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
    mMainPassCB.gSkyBoxIndex = mSkyBindlessIndex;

    mMainPassCB.gNormalMapsEnabled = mNormalMapsEnabled;
    mMainPassCB.gReflectionsEnabled = mReflectionsEnabled;
    mMainPassCB.gShadowsEnabled = false;
    mMainPassCB.gSsaoEnabled = false;

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

    UpdateCubeMapFacePassCBs();
}

void DynamicCubeMap::UpdateCubeMapFacePassCBs()
{
    for(int i = 0; i < 6; ++i)
    {
        PerPassCB cubeFacePassCB = mMainPassCB;

        XMMATRIX view = mCubeMapCamera[i].GetView();
        XMMATRIX proj = mCubeMapCamera[i].GetProj();

        XMMATRIX viewProj = XMMatrixMultiply(view, proj);
        XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
        XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
        XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

        XMStoreFloat4x4(&cubeFacePassCB.gView, XMMatrixTranspose(view));
        XMStoreFloat4x4(&cubeFacePassCB.gInvView, XMMatrixTranspose(invView));
        XMStoreFloat4x4(&cubeFacePassCB.gProj, XMMatrixTranspose(proj));
        XMStoreFloat4x4(&cubeFacePassCB.gInvProj, XMMatrixTranspose(invProj));
        XMStoreFloat4x4(&cubeFacePassCB.gViewProj, XMMatrixTranspose(viewProj));
        XMStoreFloat4x4(&cubeFacePassCB.gInvViewProj, XMMatrixTranspose(invViewProj));
        cubeFacePassCB.gEyePosW = mCubeMapCamera[i].GetPosition3f();
        cubeFacePassCB.gRenderTargetSize = XMFLOAT2((float)CubeMapSize, (float)CubeMapSize);
        cubeFacePassCB.gInvRenderTargetSize = XMFLOAT2(1.0f / CubeMapSize, 1.0f / CubeMapSize);

        auto currPassCB = mCurrFrameResource->PassCB.get();

        // Cube map pass cbuffers are stored in elements 1-6.
        currPassCB->CopyData(OffsetToCubeFace0 + i, cubeFacePassCB);
    }
}

void DynamicCubeMap::LoadTextures()
{
    TextureLib& texLib = TextureLib::GetLib();
    texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
}

void DynamicCubeMap::LoadGeometry()
{
    std::unique_ptr<MeshGeometry> shapeGeo = d3dUtil::BuildShapeGeometry(md3dDevice.Get(), *mUploadBatch.get());
    if(shapeGeo != nullptr)
    {
        mGeometries[shapeGeo->Name] = std::move(shapeGeo);
    }

    std::unique_ptr<MeshGeometry> skullGeo = d3dUtil::BuildSkullGeometry(md3dDevice.Get(), *mUploadBatch.get());
    if(skullGeo != nullptr)
    {
        mGeometries[skullGeo->Name] = std::move(skullGeo);
    }

    std::unique_ptr<MeshGeometry> columnSquare = d3dUtil::LoadSimpleModelGeometry(
        md3dDevice.Get(), *mUploadBatch.get(), "Models/columnSquare.m3d", "columnSquare");

    if(columnSquare != nullptr)
    {
        mGeometries[columnSquare->Name] = std::move(columnSquare);
    }

    std::unique_ptr<MeshGeometry> columnSquareBroken = d3dUtil::LoadSimpleModelGeometry(
        md3dDevice.Get(), *mUploadBatch.get(), "Models/columnSquareBroken.m3d", "columnSquareBroken");

    if(columnSquareBroken != nullptr)
    {
        mGeometries[columnSquareBroken->Name] = std::move(columnSquareBroken);
    }

    std::unique_ptr<MeshGeometry> columnRound = d3dUtil::LoadSimpleModelGeometry(
        md3dDevice.Get(), *mUploadBatch.get(), "Models/columnRound.m3d", "columnRound");

    if(columnRound != nullptr)
    {
        mGeometries[columnRound->Name] = std::move(columnRound);
    }

    std::unique_ptr<MeshGeometry> columnRoundBroken = d3dUtil::LoadSimpleModelGeometry(
        md3dDevice.Get(), *mUploadBatch.get(), "Models/columnRoundBroken.m3d", "columnRoundBroken");

    if(columnRoundBroken != nullptr)
    {
        mGeometries[columnRoundBroken->Name] = std::move(columnRoundBroken);
    }

    std::unique_ptr<MeshGeometry> orbBase = d3dUtil::LoadSimpleModelGeometry(
        md3dDevice.Get(), *mUploadBatch.get(), "Models/orbBase.m3d", "orbBase");

    if(orbBase != nullptr)
    {
        mGeometries[orbBase->Name] = std::move(orbBase);
    }
}

void DynamicCubeMap::BuildRootSignature()
{
    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER gfxRootParameters[GFX_ROOT_ARG_COUNT];

    // Perfomance TIP: Order from most frequent to least frequent.
    gfxRootParameters[GFX_ROOT_ARG_OBJECT_CBV].InitAsConstantBufferView(0);
    gfxRootParameters[GFX_ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);
    gfxRootParameters[GFX_ROOT_ARG_SKINNED_CBV].InitAsConstantBufferView(2);
    gfxRootParameters[GFX_ROOT_ARG_MATERIAL_SRV].InitAsShaderResourceView(0);
    gfxRootParameters[GFX_ROOT_ARG_INSTANCEDATA_SRV].InitAsShaderResourceView(1);

    CD3DX12_ROOT_SIGNATURE_DESC gfxRootSigDesc(
        GFX_ROOT_ARG_COUNT, 
        gfxRootParameters,
        0, nullptr, // static samplers
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&gfxRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if(errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));

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

void DynamicCubeMap::BuildCbvSrvUavDescriptorHeap()
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

	//
	// Fill out the heap with actual descriptors.
	//

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

    mRandomTexBindlessIndex = texLib["randomTex1024"]->BindlessIndex;
    mSkyBindlessIndex = texLib["skyCubeMap"]->BindlessIndex;

    CD3DX12_CPU_DESCRIPTOR_HANDLE cubeRtvHandles[6];
    for(int i = 0; i < 6; ++i)
        cubeRtvHandles[i] = mRtvHeap.CpuHandle(RTV_CUBE_FACE0 + i);

    mCubeDSV = mDsvHeap.CpuHandle(DSV_DYNAMIC_CUBEMAP);

    mDynamicCubeMap->BuildDescriptors(cubeRtvHandles);
}

void DynamicCubeMap::BuildCubeDepthStencil()
{
    // Create the depth/stencil buffer and view.
    D3D12_RESOURCE_DESC depthStencilDesc;
    depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthStencilDesc.Alignment = 0;
    depthStencilDesc.Width = CubeMapSize;
    depthStencilDesc.Height = CubeMapSize;
    depthStencilDesc.DepthOrArraySize = 1;
    depthStencilDesc.MipLevels = 1;
    depthStencilDesc.Format = mDepthStencilFormat;
    depthStencilDesc.SampleDesc.Count = 1;
    depthStencilDesc.SampleDesc.Quality = 0;
    depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear;
    optClear.Format = mDepthStencilFormat;
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &depthStencilDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &optClear,
        IID_PPV_ARGS(mCubeDepthStencilBuffer.GetAddressOf())));

    // Create descriptor to mip level 0 of entire resource using the format of the resource.
    md3dDevice->CreateDepthStencilView(mCubeDepthStencilBuffer.Get(), nullptr, mCubeDSV);
}

void DynamicCubeMap::BuildShadersAndInputLayout()
{
    ShaderLib::GetLib().Init(md3dDevice.Get());
}

void DynamicCubeMap::BuildPSOs()
{
    PsoLib::GetLib().Init(
        md3dDevice.Get(),
        mBackBufferFormat,
        mDepthStencilFormat,
        SsaoAmbientMapFormat,
        SceneNormalMapFormat,
        mRootSignature.Get());
}

void DynamicCubeMap::BuildFrameResources()
{
    constexpr UINT mainPassCount = 1;
    constexpr UINT cubeMapPassCount = 6;
    constexpr UINT passCount = mainPassCount + cubeMapPassCount;
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(
            std::make_unique<FrameResource>(md3dDevice.Get(),
            passCount, (UINT)mAllRitems.size(), MaterialLib::GetLib().GetMaterialCount()));
    }
}

void DynamicCubeMap::BuildMaterials()
{
    MaterialLib::GetLib().Init(md3dDevice.Get());
}

RenderItem* DynamicCubeMap::AddRenderItem(RenderLayer layer, const XMFLOAT4X4& world, const XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs)
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

void DynamicCubeMap::BuildRenderItems()
{
    MaterialLib& matLib = MaterialLib::GetLib();

    XMFLOAT4X4 worldTransform;
    XMFLOAT4X4 texTransform;

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Sky, worldTransform, texTransform, matLib["sky"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(0.4f, 0.4f, 0.4f) * XMMatrixTranslation(0.0f, 1.0f, 0.0f));
    texTransform = MathHelper::Identity4x4();
    mSkullRitem = AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["skullMatMatte"], mGeometries["skullGeo"].get(), mGeometries["skullGeo"]->DrawArgs["skull"]);

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(6.0f, 6.0f, 6.0f) * XMMatrixTranslation(0.0f, 0.0f, 0.0f));
    XMStoreFloat4x4(&texTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["orbBase"], mGeometries["orbBase"].get(), mGeometries["orbBase"]->DrawArgs["subset0"]);

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(0.0f, 1.75f, 0.0f));
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror1"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

    worldTransform = MathHelper::Identity4x4();
    XMStoreFloat4x4(&texTransform, XMMatrixScaling(6.0f, 6.0f, 1.0f));
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["stoneFloor"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"]);

    XMMATRIX falledColumnTransform0 =
        XMMatrixRotationZ(-0.54f * XM_PI) *
        XMMatrixRotationY(0.15f * XM_PI) *
        XMMatrixScaling(1.0f, 1.0f, 1.0f) *
        XMMatrixTranslation(-3.0f, 0.35f, 3.0f);
    XMStoreFloat4x4(&worldTransform, falledColumnTransform0);
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);

    XMMATRIX falledColumnTransform1 =
        XMMatrixRotationZ(-0.54f * XM_PI) *
        XMMatrixRotationY(0.75f * XM_PI) *
        XMMatrixScaling(1.0f, 1.0f, 1.0f) *
        XMMatrixTranslation(1.5f, 0.35f, -4.0f);
    XMStoreFloat4x4(&worldTransform, falledColumnTransform1);
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);


    for(int i = 0; i < 5; ++i)
    {
        bool isLeftColumnBroken = (i == 2);
        bool isRightColumnBroken = (i == 0 || i == 4);

        std::string columnNameLeft = isLeftColumnBroken ? "columnSquareBroken" : "columnSquare";
        std::string columnNameRight = isRightColumnBroken ? "columnSquareBroken" : "columnSquare";

        XMStoreFloat4x4(&texTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
        XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(-5.0f, 0.0f, -10.0f + i * 5.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameLeft].get(), mGeometries[columnNameLeft]->DrawArgs["subset0"]);

        XMStoreFloat4x4(&texTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
        XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(+5.0f, 0.0f, -10.0f + i * 5.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameRight].get(), mGeometries[columnNameRight]->DrawArgs["subset0"]);

        if(!isLeftColumnBroken)
        {
            texTransform = MathHelper::Identity4x4();
            XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(-5.0f, 4.0f, -10.0f + i * 5.0f));
            AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
        }

        if(!isRightColumnBroken)
        {
            texTransform = MathHelper::Identity4x4();
            XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(+5.0f, 4.0f, -10.0f + i * 5.0f));
            AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
        }
    }
}

void DynamicCubeMap::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
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

void DynamicCubeMap::DrawSceneToCubeMap()
{
    PsoLib& psoLib = PsoLib::GetLib();
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    SamplerHeap& samHeap = SamplerHeap::Get();

    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), psoLib["opaque"]));

    ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavHeap.GetD3dHeap(), samHeap.GetD3dHeap() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    // Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
    // set as a root descriptor.
    auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
    mCommandList->SetGraphicsRootShaderResourceView(GFX_ROOT_ARG_MATERIAL_SRV, matBuffer->GetGPUVirtualAddress());

    mCommandList->RSSetViewports(1, &mDynamicCubeMap->Viewport());
    mCommandList->RSSetScissorRects(1, &mDynamicCubeMap->ScissorRect());

    // Change to RENDER_TARGET.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDynamicCubeMap->Resource(),
                                  D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

    UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerPassCB));

    // For each cube map face.
    for(int i = 0; i < 6; ++i)
    {
        // Clear the back buffer and depth buffer.
        mCommandList->ClearRenderTargetView(mDynamicCubeMap->Rtv(i), Colors::Black, 0, nullptr);
        mCommandList->ClearDepthStencilView(mCubeDSV, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

        // Specify the buffers we are going to render to.
        mCommandList->OMSetRenderTargets(1, &mDynamicCubeMap->Rtv(i), true, &mCubeDSV);

        // Bind the pass constant buffer for this cube map face so we use 
        // the right view/proj matrix for this cube face.
        auto passCB = mCurrFrameResource->PassCB->Resource();
        D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = passCB->GetGPUVirtualAddress() + (OffsetToCubeFace0+i)*passCBByteSize;
        mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCBAddress);

        mCommandList->SetPipelineState(psoLib["opaque"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

        mCommandList->SetPipelineState(psoLib["sky"]);
        DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);
    }

    // Change back to GENERIC_READ so we can read the texture in a shader.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDynamicCubeMap->Resource(),
                                  D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

    // Done recording commands.
    ThrowIfFailed(mCommandList->Close());

    mLinearAllocator->Commit(mCommandQueue.Get());

    // Add the command list to the queue for execution.
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
}

void DynamicCubeMap::BuildCubeFaceCamera(float x, float y, float z)
{
    // Generate the cube map about the given position.
    XMFLOAT3 center(x, y, z);
    XMFLOAT3 worldUp(0.0f, 1.0f, 0.0f);

    // Look along each coordinate axis.
    XMFLOAT3 targets[6] =
    {
        XMFLOAT3(x + 1.0f, y, z), // +X
        XMFLOAT3(x - 1.0f, y, z), // -X
        XMFLOAT3(x, y + 1.0f, z), // +Y
        XMFLOAT3(x, y - 1.0f, z), // -Y
        XMFLOAT3(x, y, z + 1.0f), // +Z
        XMFLOAT3(x, y, z - 1.0f)  // -Z
    };

    // Use world up vector (0,1,0) for all directions except +Y/-Y.  In these cases, we
    // are looking down +Y or -Y, so we need a different "up" vector.
    XMFLOAT3 ups[6] =
    {
        XMFLOAT3(0.0f, 1.0f, 0.0f),  // +X
        XMFLOAT3(0.0f, 1.0f, 0.0f),  // -X
        XMFLOAT3(0.0f, 0.0f, -1.0f), // +Y
        XMFLOAT3(0.0f, 0.0f, +1.0f), // -Y
        XMFLOAT3(0.0f, 1.0f, 0.0f),	 // +Z
        XMFLOAT3(0.0f, 1.0f, 0.0f)	 // -Z
    };

    for(int i = 0; i < 6; ++i)
    {
        mCubeMapCamera[i].LookAt(center, targets[i], ups[i]);
        mCubeMapCamera[i].SetLens(0.5f*XM_PI, 1.0f, 0.1f, 1000.0f);
        mCubeMapCamera[i].UpdateViewMatrix();
    }
}