//***************************************************************************************
// HybridRayTracingApp.cpp by Frank Luna (C) 2023 All Rights Reserved.
//***************************************************************************************

#include "HybridRayTracingApp.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

//
// Define named offsets into descriptor heaps for readability.
//

enum RtvOffsets
{
    // Start after swapchain buffers.
    RTV_NORMALMAP = D3DApp::SwapChainBufferCount,
    RTV_COUNT
};

enum DsvOffsets
{
    DSV_MAINVIEW = 0,
    DSV_SHADOWMAP,
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
        HybridRayTracingApp theApp(hInstance);
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

HybridRayTracingApp::HybridRayTracingApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
    // Estimate the scene bounding sphere manually since we know how the scene was constructed.
    // The grid is the "widest object" with a width of 20 and depth of 30.0f, and centered at
    // the world space origin.  In general, you need to loop over every world space vertex
    // position and compute the bounding sphere.
    mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
    mSceneBounds.Radius = sqrtf(10.0f*10.0f + 15.0f*15.0f);
}

HybridRayTracingApp::~HybridRayTracingApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool HybridRayTracingApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

	mCamera.SetPosition(0.0f, 2.0f, -15.0f);

    mPrepass = std::make_unique<Prepass>(md3dDevice.Get());
 
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
    BuildShaders();
	BuildMaterials();
    InitRayTracing();
    BuildRenderItems();
    BuildFrameResources();
    BuildPSOs();

    // Block until the upload work is complete.
    result.wait();

    // Build ray tracing structs on GPU and wait for it to be done. 
    // In a large app where it might take a while to build, we could 
    // refactor to do other async work here while waiting for build.
    mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr);
    mRayTracer->ExecuteBuildAccelerationStructureCommands(mCommandQueue.Get());

    return true;
}

void HybridRayTracingApp::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_COUNT);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2);
}
 
void HybridRayTracingApp::OnResize()
{
    D3DApp::OnResize();

	mCamera.SetLens(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);

    if(mPrepass != nullptr)
    {
        mPrepass->OnResize(mClientWidth, mClientHeight, mDepthStencilBuffer.Get());
    }

    if(mRayTracer != nullptr)
    {
        mRayTracer->OnResize(mClientWidth, mClientHeight);
    }
}

void HybridRayTracingApp::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);

    // Cycle through the circular frame resource array.
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    // Has the GPU finished processing the commands of the current frame resource?
    // If not, wait until the GPU has completed commands up to this fence point.
    if(mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, nullptr, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    //
    // Animate the lights (and hence shadows).
    //

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
    UpdateShadowTransform(gt);
	UpdateMainPassCB(gt);
}

void HybridRayTracingApp::Draw(const GameTimer& gt)
{
    PsoLib& psoLib = PsoLib::GetLib();
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    SamplerHeap& samHeap = SamplerHeap::Get();

    UpdateImgui(gt);

    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(cmdListAlloc->Reset());

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

    //
    // Normal/depth pass.
    //

    DrawNormalsAndDepth();

    //
    // Compute hybrid ray tracing.
    // 

    auto passCB = mCurrFrameResource->PassCB->Resource();

    if(mRayTracer != nullptr)
    {
       mRayTracer->Draw(passCB, matBuffer);
    }

    //
    // Main rendering pass.
    //

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear the back buffer and depth buffer.
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);

    // WE ALREADY WROTE THE DEPTH INFO TO THE DEPTH BUFFER IN DrawNormalsAndDepth,
    // SO DO NOT CLEAR DEPTH.

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

	mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

    mCommandList->SetPipelineState(psoLib["opaque_hybrid_rt"]);
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

void HybridRayTracingApp::UpdateImgui(const GameTimer& gt)
{
    D3DApp::UpdateImgui(gt);

    //
    // Define a panel to render GUI elements.
    // 
    ImGui::Begin("Options");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

    ImGui::Checkbox("NormalMaps", &mNormalMapsEnabled);
    ImGui::Checkbox("Reflections", &mReflectionsEnabled);
    ImGui::Checkbox("Shadows", &mShadowsEnabled);
    ImGui::SliderFloat("LightAngle", &mLightRotationAngle, 0.0f, 2.0f * XM_PI);

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

void HybridRayTracingApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        SetCapture(mhMainWnd);
    }
}

void HybridRayTracingApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void HybridRayTracingApp::OnMouseMove(WPARAM btnState, int x, int y)
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
 
void HybridRayTracingApp::OnKeyboardInput(const GameTimer& gt)
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
 
void HybridRayTracingApp::AnimateMaterials(const GameTimer& gt)
{
	
}

void HybridRayTracingApp::UpdatePerObjectCB(const GameTimer& gt)
{
    // Update per object constants once per frame so the data can be shared across different render passes.
    for (auto& ri : mAllRitems)
    {
        XMStoreFloat4x4(&ri->ObjectConstants.gWorld, XMMatrixTranspose(XMLoadFloat4x4(&ri->World)));
        XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, XMMatrixTranspose(XMLoadFloat4x4(&ri->TexTransform)));
        ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;
        ri->ObjectConstants.gCubeMapIndex = mSkyBindlessIndex;

        // Need to hold handle until we submit work to GPU.
        ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectConstants);
    }
}

void HybridRayTracingApp::UpdateMaterialBuffer(const GameTimer& gt)
{
	auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();

    MaterialLib& matLib = MaterialLib::GetLib();
	for(auto& e : matLib.GetCollection())
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
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
            matData.TransparencyWeight = mat->TransparencyWeight;
            matData.IndexOfRefraction = mat->IndexOfRefraction;

			currMaterialBuffer->CopyData(mat->MatIndex, matData);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void HybridRayTracingApp::UpdateShadowTransform(const GameTimer& gt)
{
    // Only the first "main" light casts a shadow.
    XMVECTOR lightDir = XMLoadFloat3(&mRotatedLightDirections[0]);
    XMVECTOR lightPos = -2.0f*mSceneBounds.Radius*lightDir;
    XMVECTOR targetPos = XMLoadFloat3(&mSceneBounds.Center);
    XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

    XMStoreFloat3(&mLightPosW, lightPos);

    // Transform bounding sphere to light space.
    XMFLOAT3 sphereCenterLS;
    XMStoreFloat3(&sphereCenterLS, XMVector3TransformCoord(targetPos, lightView));

    // Ortho frustum in light space encloses scene.
    float l = sphereCenterLS.x - mSceneBounds.Radius;
    float b = sphereCenterLS.y - mSceneBounds.Radius;
    float n = sphereCenterLS.z - mSceneBounds.Radius;
    float r = sphereCenterLS.x + mSceneBounds.Radius;
    float t = sphereCenterLS.y + mSceneBounds.Radius;
    float f = sphereCenterLS.z + mSceneBounds.Radius;

    mLightNearZ = n;
    mLightFarZ = f;
    XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    XMMATRIX T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    XMMATRIX S = lightView*lightProj*T;
    XMStoreFloat4x4(&mLightView, lightView);
    XMStoreFloat4x4(&mLightProj, lightProj);
    XMStoreFloat4x4(&mShadowTransform, S);
}

void HybridRayTracingApp::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX proj = mCamera.GetProj();

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    XMMATRIX T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    XMMATRIX viewProjTex = XMMatrixMultiply(viewProj, T);

    XMMATRIX shadowTransform = XMLoadFloat4x4(&mShadowTransform);

	XMStoreFloat4x4(&mMainPassCB.gView, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.gInvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.gProj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.gInvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.gViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.gInvViewProj, XMMatrixTranspose(invViewProj));
    XMStoreFloat4x4(&mMainPassCB.gShadowTransform, XMMatrixTranspose(shadowTransform));
    XMStoreFloat4x4(&mMainPassCB.gViewProjTex, XMMatrixTranspose(viewProjTex));

	mMainPassCB.gEyePosW = mCamera.GetPosition3f();
	mMainPassCB.gRenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.gInvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.gNearZ = 1.0f;
	mMainPassCB.gFarZ = 1000.0f;
	mMainPassCB.gTotalTime = gt.TotalTime();
	mMainPassCB.gDeltaTime = gt.DeltaTime();
	mMainPassCB.gAmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
    mMainPassCB.gRandomTexIndex = mRandomTexBindlessIndex;
    mMainPassCB.gSkyBoxIndex = mSkyBindlessIndex;

    mMainPassCB.gSceneDepthMapIndex = mPrepass->GetSceneDepthMapBindlessIndex();
    mMainPassCB.gSceneNormalMapIndex = mPrepass->GetSceneNormalMapBindlessIndex();
    mMainPassCB.gReflectionMapUavIndex = mRayTracer->GetReflectionMapUavIndex();
    mMainPassCB.gReflectionMapSrvIndex = mRayTracer->GetReflectionMapSrvIndex();
    mMainPassCB.gDebugTexIndex = mRayTracer->GetReflectionMapSrvIndex();

    mMainPassCB.gNormalMapsEnabled = mNormalMapsEnabled;
    mMainPassCB.gReflectionsEnabled = mReflectionsEnabled;
    mMainPassCB.gShadowsEnabled = mShadowsEnabled;

    mMainPassCB.gLights[0].Direction = mRotatedLightDirections[0];
    mMainPassCB.gLights[0].Strength = { 0.8f, 0.8f, 0.8f };
    mMainPassCB.gLights[1].Direction = mRotatedLightDirections[1];
    mMainPassCB.gLights[1].Strength = { 0.1f, 0.1f, 0.1f };
    mMainPassCB.gLights[2].Direction = mRotatedLightDirections[2];
    mMainPassCB.gLights[2].Strength = { 0.1f, 0.1f, 0.1f };
 
	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void HybridRayTracingApp::LoadTextures()
{
    TextureLib& texLib = TextureLib::GetLib();
    texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
}

void HybridRayTracingApp::LoadGeometry()
{
    constexpr bool useIndex32 = true;
    std::unique_ptr<MeshGeometry> shapeGeo = d3dUtil::BuildShapeGeometry(md3dDevice.Get(), *mUploadBatch.get(), useIndex32);
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
        md3dDevice.Get(), *mUploadBatch.get(),
        "Models/columnSquare.m3d", "columnSquare",
        useIndex32);

    if(columnSquare != nullptr)
    {
        mGeometries[columnSquare->Name] = std::move(columnSquare);
    }

    std::unique_ptr<MeshGeometry> columnSquareBroken = d3dUtil::LoadSimpleModelGeometry(
        md3dDevice.Get(), *mUploadBatch.get(), 
        "Models/columnSquareBroken.m3d", "columnSquareBroken", 
        useIndex32);

    if(columnSquareBroken != nullptr)
    {
        mGeometries[columnSquareBroken->Name] = std::move(columnSquareBroken);
    }

    std::unique_ptr<MeshGeometry> columnRound = d3dUtil::LoadSimpleModelGeometry(
        md3dDevice.Get(), *mUploadBatch.get(), 
        "Models/columnRound.m3d", "columnRound",
        useIndex32);

    if(columnRound != nullptr)
    {
        mGeometries[columnRound->Name] = std::move(columnRound);
    }

    std::unique_ptr<MeshGeometry> columnRoundBroken = d3dUtil::LoadSimpleModelGeometry(
        md3dDevice.Get(), *mUploadBatch.get(), 
        "Models/columnRoundBroken.m3d", "columnRoundBroken",
        useIndex32);

    if(columnRoundBroken != nullptr)
    {
        mGeometries[columnRoundBroken->Name] = std::move(columnRoundBroken);
    }

    std::unique_ptr<MeshGeometry> orbBase = d3dUtil::LoadSimpleModelGeometry(
        md3dDevice.Get(), *mUploadBatch.get(), 
        "Models/orbBase.m3d", "orbBase",
        useIndex32);

    if(orbBase != nullptr)
    {
        mGeometries[orbBase->Name] = std::move(orbBase);
    }
}

void HybridRayTracingApp::BuildRootSignature()
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
        GFX_ROOT_ARG_COUNT, gfxRootParameters,
        0, nullptr, // static samplers
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    // create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
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
}

void HybridRayTracingApp::BuildCbvSrvUavDescriptorHeap()
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

    mPrepass->AllocateDescriptors(mRtvHeap.CpuHandle(RTV_NORMALMAP));
    mPrepass->OnResize(mClientWidth, mClientHeight, mDepthStencilBuffer.Get());

    //
    // Ray tracing needs descriptors to the geometry buffers.
    //
    mShapeVertexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
    mShapeIndexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
    mOrbBaseVertexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
    mOrbBaseIndexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
    mColumnRoundBrokenVertexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
    mColumnRoundBrokenIndexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
    mColumnSquareBrokenVertexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
    mColumnSquareBrokenIndexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
    mColumnSquareVertexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();
    mColumnSquareIndexBufferBindlessIndex = cbvSrvUavHeap.NextFreeIndex();

    uint32_t indexByteSize = mGeometries["shapeGeo"]->IndexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
    uint32_t vertexCount = mGeometries["shapeGeo"]->VertexBufferByteSize / mGeometries["shapeGeo"]->VertexByteStride;
    uint32_t indexCount = mGeometries["shapeGeo"]->IndexBufferByteSize / indexByteSize;

    CreateBufferSrv(md3dDevice.Get(), 0, vertexCount, sizeof(RTVertex), mGeometries["shapeGeo"]->VertexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mShapeVertexBufferBindlessIndex));
    CreateBufferSrv(md3dDevice.Get(), 0, indexCount, indexByteSize, mGeometries["shapeGeo"]->IndexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mShapeIndexBufferBindlessIndex));

    indexByteSize = mGeometries["orbBase"]->IndexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
    vertexCount = mGeometries["orbBase"]->VertexBufferByteSize / mGeometries["orbBase"]->VertexByteStride;
    indexCount = mGeometries["orbBase"]->IndexBufferByteSize / indexByteSize;

    CreateBufferSrv(md3dDevice.Get(), 0, vertexCount, sizeof(RTVertex), mGeometries["orbBase"]->VertexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mOrbBaseVertexBufferBindlessIndex));
    CreateBufferSrv(md3dDevice.Get(), 0, indexCount, indexByteSize, mGeometries["orbBase"]->IndexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mOrbBaseIndexBufferBindlessIndex));

    indexByteSize = mGeometries["columnRoundBroken"]->IndexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
    vertexCount = mGeometries["columnRoundBroken"]->VertexBufferByteSize / mGeometries["columnRoundBroken"]->VertexByteStride;
    indexCount = mGeometries["columnRoundBroken"]->IndexBufferByteSize / indexByteSize;

    CreateBufferSrv(md3dDevice.Get(), 0, vertexCount, sizeof(RTVertex), mGeometries["columnRoundBroken"]->VertexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnRoundBrokenVertexBufferBindlessIndex));
    CreateBufferSrv(md3dDevice.Get(), 0, indexCount, indexByteSize, mGeometries["columnRoundBroken"]->IndexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnRoundBrokenIndexBufferBindlessIndex));

    indexByteSize = mGeometries["columnSquareBroken"]->IndexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
    vertexCount = mGeometries["columnSquareBroken"]->VertexBufferByteSize / mGeometries["columnSquareBroken"]->VertexByteStride;
    indexCount = mGeometries["columnSquareBroken"]->IndexBufferByteSize / indexByteSize;

    CreateBufferSrv(md3dDevice.Get(), 0, vertexCount, sizeof(RTVertex), mGeometries["columnSquareBroken"]->VertexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnSquareBrokenVertexBufferBindlessIndex));
    CreateBufferSrv(md3dDevice.Get(), 0, indexCount, indexByteSize, mGeometries["columnSquareBroken"]->IndexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnSquareBrokenIndexBufferBindlessIndex));

    indexByteSize = mGeometries["columnSquare"]->IndexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
    vertexCount = mGeometries["columnSquare"]->VertexBufferByteSize / mGeometries["columnSquare"]->VertexByteStride;
    indexCount = mGeometries["columnSquare"]->IndexBufferByteSize / indexByteSize;

    CreateBufferSrv(md3dDevice.Get(), 0, vertexCount, sizeof(RTVertex), mGeometries["columnSquare"]->VertexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnSquareVertexBufferBindlessIndex));
    CreateBufferSrv(md3dDevice.Get(), 0, indexCount, indexByteSize, mGeometries["columnSquare"]->IndexBufferGPU.Get(), cbvSrvUavHeap.CpuHandle(mColumnSquareIndexBufferBindlessIndex));
}

void HybridRayTracingApp::BuildShaders()
{
    ShaderLib::GetLib().Init(md3dDevice.Get());
}

void HybridRayTracingApp::BuildPSOs()
{
    PsoLib::GetLib().Init(
        md3dDevice.Get(),
        mBackBufferFormat,
        mDepthStencilFormat,
        SsaoAmbientMapFormat,
        SceneNormalMapFormat,
        mRootSignature.Get(),
        nullptr);
}

void HybridRayTracingApp::BuildFrameResources()
{
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            2, (UINT)mAllRitems.size(), MaterialLib::GetLib().GetMaterialCount()));
    }
}
 
void HybridRayTracingApp::BuildMaterials()
{
    MaterialLib::GetLib().Init(md3dDevice.Get());
}

void HybridRayTracingApp::InitRayTracing()
{
    ShaderLib& shaderLib = ShaderLib::GetLib();

    mRayTracer = std::make_unique<HybridRayTracer>(
        md3dDevice.Get(),
        mCommandList.Get(),
        shaderLib["hybridReflectionsRTLib"],
        mClientWidth, mClientHeight);

    // For simplicity, we use 32-bit indices in this demo. But you can use 
    // a ByteAddressBuffer and pack two 16-bit indices per dword.
    auto MakeRtModel = [](const MeshGeometry* geo, const SubmeshGeometry& drawArgs, uint32_t vbIndex, uint32_t ibIndex) ->HybridRayTracer::RTModelDef
    {
        HybridRayTracer::RTModelDef model;

        model.VertexBuffer = geo->VertexBufferGPU.Get();
        model.IndexBuffer = geo->IndexBufferGPU.Get();
        model.VertexBufferBindlessIndex = vbIndex;
        model.IndexBufferBindlessIndex = ibIndex;
        model.IndexFormat = DXGI_FORMAT_R32_UINT;
        model.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        model.IndexCount = drawArgs.IndexCount;
        model.VertexCount = drawArgs.VertexCount;
        model.StartIndexLocation = drawArgs.StartIndexLocation;
        model.BaseVertexLocation = drawArgs.BaseVertexLocation;
        model.VertexSizeInBytes = sizeof(RTVertex);
        model.IndexSizeInBytes = sizeof(uint32_t);

        return model;
    };

    assert(mShapeVertexBufferBindlessIndex != -1);
    assert(mShapeIndexBufferBindlessIndex != -1);
    assert(mOrbBaseVertexBufferBindlessIndex != -1);
    assert(mOrbBaseIndexBufferBindlessIndex != -1);
    assert(mColumnRoundBrokenVertexBufferBindlessIndex != -1);
    assert(mColumnRoundBrokenIndexBufferBindlessIndex != -1);
    assert(mColumnSquareBrokenVertexBufferBindlessIndex != -1);
    assert(mColumnSquareBrokenIndexBufferBindlessIndex != -1);
    assert(mColumnSquareVertexBufferBindlessIndex != -1);
    assert(mColumnSquareIndexBufferBindlessIndex != -1);

    // Define RTModelDefs to the same geometry we use for rasterization.
    HybridRayTracer::RTModelDef grid = MakeRtModel(
        mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"], 
        mShapeVertexBufferBindlessIndex, mShapeIndexBufferBindlessIndex);
    HybridRayTracer::RTModelDef sphere = MakeRtModel(
        mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"],
        mShapeVertexBufferBindlessIndex, mShapeIndexBufferBindlessIndex);
    HybridRayTracer::RTModelDef orbBaseModel = MakeRtModel(
        mGeometries["orbBase"].get(), mGeometries["orbBase"]->DrawArgs["subset0"],
        mOrbBaseVertexBufferBindlessIndex, mOrbBaseIndexBufferBindlessIndex);
    HybridRayTracer::RTModelDef columnRoundBrokenModel = MakeRtModel(
        mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"],
        mColumnRoundBrokenVertexBufferBindlessIndex, mColumnRoundBrokenIndexBufferBindlessIndex);
    HybridRayTracer::RTModelDef columnSquareBrokenModel = MakeRtModel(
        mGeometries["columnSquareBroken"].get(), mGeometries["columnSquareBroken"]->DrawArgs["subset0"],
        mColumnSquareBrokenVertexBufferBindlessIndex, mColumnSquareBrokenIndexBufferBindlessIndex);
    HybridRayTracer::RTModelDef columnSquareModel = MakeRtModel(
        mGeometries["columnSquare"].get(), mGeometries["columnSquare"]->DrawArgs["subset0"],
        mColumnSquareVertexBufferBindlessIndex, mColumnSquareIndexBufferBindlessIndex);

    mRayTracer->AddModel("gridModel", grid);
    mRayTracer->AddModel("sphereModel", sphere);
    mRayTracer->AddModel("orbBaseModel", orbBaseModel);
    mRayTracer->AddModel("columnRoundBrokenModel", columnRoundBrokenModel);
    mRayTracer->AddModel("columnSquareBrokenModel", columnSquareBrokenModel);
    mRayTracer->AddModel("columnSquareModel", columnSquareModel);
}

void HybridRayTracingApp::AddRenderItem(RenderLayer layer, const XMFLOAT4X4& world, const XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs)
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

void HybridRayTracingApp::BuildRenderItems()
{
    MaterialLib& matLib = MaterialLib::GetLib();

    XMFLOAT4X4 worldTransform;
    XMFLOAT4X4 texTransform;

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Sky, worldTransform, texTransform, matLib["sky"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(6.0f, 6.0f, 6.0f) * XMMatrixTranslation(0.0f, 0.0f, 0.0f));
    XMStoreFloat4x4(&texTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["orbBase"], mGeometries["orbBase"].get(), mGeometries["orbBase"]->DrawArgs["subset0"]);
    mRayTracer->AddInstance("orbBaseModel", worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["orbBase"]->MatIndex);
    
    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(0.0f, 1.75f, 0.0f));
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror1"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
    mRayTracer->AddInstance("sphereModel", worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["mirror1"]->MatIndex);
    
    worldTransform = MathHelper::Identity4x4();
    XMStoreFloat4x4(&texTransform, XMMatrixScaling(6.0f, 6.0f, 1.0f));
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["stoneFloor"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"]);
    mRayTracer->AddInstance("gridModel", worldTransform, XMFLOAT2(6.0f, 6.0f), matLib["stoneFloor"]->MatIndex);

    XMMATRIX falledColumnTransform0 =
        XMMatrixRotationZ(-0.54f * XM_PI) *
        XMMatrixRotationY(0.15f * XM_PI) *
        XMMatrixScaling(1.0f, 1.0f, 1.0f) *
        XMMatrixTranslation(-3.0f, 0.35f, 3.0f);
    XMStoreFloat4x4(&worldTransform, falledColumnTransform0);
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);
    mRayTracer->AddInstance("columnRoundBrokenModel", worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["columnRound"]->MatIndex);

    XMMATRIX falledColumnTransform1 =
        XMMatrixRotationZ(-0.54f * XM_PI) *
        XMMatrixRotationY(0.75f * XM_PI) *
        XMMatrixScaling(1.0f, 1.0f, 1.0f) *
        XMMatrixTranslation(1.5f, 0.35f, -4.0f);
    XMStoreFloat4x4(&worldTransform, falledColumnTransform1);
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);
    mRayTracer->AddInstance("columnRoundBrokenModel", worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["columnRound"]->MatIndex);
    
	for(int i = 0; i < 5; ++i)
	{
        bool isLeftColumnBroken = (i == 2);
        bool isRightColumnBroken = (i == 0 || i == 4);

        std::string columnNameLeft = isLeftColumnBroken ? "columnSquareBroken" : "columnSquare";
        std::string columnNameRight = isRightColumnBroken ? "columnSquareBroken" : "columnSquare";

        XMStoreFloat4x4(&texTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
        XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(-5.0f, 0.0f, -10.0f + i * 5.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameLeft].get(), mGeometries[columnNameLeft]->DrawArgs["subset0"]);
        mRayTracer->AddInstance(columnNameLeft + "Model", worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["columnSquare"]->MatIndex);

        XMStoreFloat4x4(&texTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
        XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(+5.0f, 0.0f, -10.0f + i * 5.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameRight].get(), mGeometries[columnNameRight]->DrawArgs["subset0"]);
        mRayTracer->AddInstance(columnNameRight + "Model", worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["columnSquare"]->MatIndex);

        if(!isLeftColumnBroken)
        {
            texTransform = MathHelper::Identity4x4();
            XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(-5.0f, 4.0f, -10.0f + i * 5.0f));
            AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
            mRayTracer->AddInstance("sphereModel", worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["mirror0"]->MatIndex);
        }

        if(!isRightColumnBroken)
        {
            texTransform = MathHelper::Identity4x4();
            XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(+5.0f, 4.0f, -10.0f + i * 5.0f));
            AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
            mRayTracer->AddInstance("sphereModel", worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["mirror0"]->MatIndex);
        }
	}

    worldTransform = MathHelper::Identity4x4();
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Debug, worldTransform, texTransform, matLib["bricks0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["quad"]);
}

void HybridRayTracingApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
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

void HybridRayTracingApp::DrawNormalsAndDepth()
{
    PsoLib& psoLib = PsoLib::GetLib();

    auto normalMap = mPrepass->GetSceneNormalMap();
    auto normalMapRtv = mPrepass->GetSceneNormalMapRtv();

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);
 
    // Change to RENDER_TARGET.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
                                  D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear the screen normal map and depth buffer.
    float clearValue[] = { 0.0f, 0.0f, 1.0f, 0.0f };
    mCommandList->ClearRenderTargetView(normalMapRtv, clearValue, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &normalMapRtv, true, &DepthStencilView());

    // Bind the constant buffer for this pass.
    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

    mCommandList->SetPipelineState(psoLib["drawBumpedWorldNormals"]);

    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    // Change back to GENERIC_READ so we can read the texture in a shader.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(normalMap,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
}

