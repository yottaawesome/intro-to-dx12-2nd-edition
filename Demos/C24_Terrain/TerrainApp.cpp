//***************************************************************************************
// TerrainApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#include "TerrainApp.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;
using namespace DirectX::SimpleMath;

const int gNumFrameResources = 3;

//
// Define named offsets into descriptor heaps for readability.
//

enum RtvOffsets
{
    // Start after swapchain buffers.
    RTV_OFFSET = D3DApp::SwapChainBufferCount,
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
        TerrainApp theApp(hInstance);
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

TerrainApp::TerrainApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
    // Estimate the scene bounding sphere manually since we know how the scene was constructed.
    // In general, you need to loop over every world space vertex position and compute the bounding sphere.
    mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
    mSceneBounds.Radius = 512; 
}

TerrainApp::~TerrainApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool TerrainApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

	mCamera.SetPosition(0.0f, -20.0f, -200.0f);
 
    // Too big, but OK for demo. For large terrain, need something like Cascaded Shadow Maps.
    mShadowMap = std::make_unique<ShadowMap>(md3dDevice.Get(), 2*4096, 2*4096);
    

    // Create the singleton.
    // GraphicsMemory::Get(md3dDevice.Get());

    // We will upload on the direct queue for the book samples, but 
    // copy queue would be better for real game.
    mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

    // Do init work that requires mUploadBatch...
	LoadTextures();
    LoadGeometry();

    mRainParticleSystem = std::make_unique<ParticleSystem>(md3dDevice.Get(), *mUploadBatch.get(), MaxRainParticleCount, false);
    mExplosionParticleSystem = std::make_unique<ParticleSystem>(md3dDevice.Get(), *mUploadBatch.get(), MaxExplosionParticleCount, false);

    Terrain::InitInfo terrainInitInfo;
    terrainInitInfo.HeightMapFilename = L"Textures/terrain/heightmap4097.raw";
    terrainInitInfo.HeightScale = 100.0f;
    terrainInitInfo.HeightOffset = -50.0f;
    terrainInitInfo.HeightmapWidth = 4097;
    terrainInitInfo.HeightmapHeight = 4097;
    terrainInitInfo.CellSpacing = 0.125f;
    terrainInitInfo.NumLayers = 7;

    mTerrain = std::make_unique<Terrain>(md3dDevice.Get(), *mUploadBatch.get(), terrainInitInfo);

    // Kick off upload work asyncronously.
    std::future<void> result = mUploadBatch->End(mCommandQueue.Get());

    // Other init work.
    BuildRootSignatures();
	BuildCbvSrvUavDescriptorHeap();
    BuildMaterials();

    TextureLib& texLib = TextureLib::GetLib();
    MaterialLib& matLib = MaterialLib::GetLib();

    mTerrain->SetMaterialLayers({
        matLib["terrainlayer0"],
        matLib["terrainlayer1"],
        matLib["terrainlayer2"],
        matLib["terrainlayer3"],
        matLib["terrainlayer4"],
        matLib["terrainlayer5"],
        matLib["terrainlayer6"]},
        texLib["blendMap0"]->BindlessIndex,
        texLib["blendMap1"]->BindlessIndex);

    BuildShaders();
    BuildRenderItems();
    BuildFrameResources();
    BuildPSOs();
    BuildCommandSignatures();

    // Block until the upload work is complete.
    result.wait();

    return true;
}

void TerrainApp::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2);
}
 
void TerrainApp::OnResize()
{
    D3DApp::OnResize();

	mCamera.SetLens(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1500.0f);
}

void TerrainApp::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);

    // Cycle through the circular frame resource array.
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    // Has the GPU finished processing the commands of the current frame resource?
    // If not, wait until the GPU has completed commands up to this fence point.
    if(mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    ReadParticleCounts(gt);

    //
    // Animate the lights (and hence shadows).
    //

    //mLightRotationAngle += 0.05f*gt.DeltaTime();
    mLightRotationAngle = 0.0f;

    XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
    for(int i = 0; i < 3; ++i)
    {
        XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&mBaseLightDirections[i]));
        lightDir = XMVector3TransformNormal(lightDir, R);
        XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
    }

	AnimateMaterials(gt);
    UpdatePerObjectCB(gt);
	UpdateMaterialBuffer(gt);
    UpdateShadowTransform(gt);
	UpdateMainPassCB(gt);
    UpdateShadowPassCB(gt);

    mExplosionParticleSystem->FrameSetup(gt);
    mRainParticleSystem->FrameSetup(gt);
    EmitExplosionParticles(gt);
    EmitRainParticles(gt);
}

void TerrainApp::Draw(const GameTimer& gt)
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

    // SetDescriptorHeaps must be called before SetGraphicsRootSignature when using HEAP_DIRECTLY_INDEXED.
    ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavHeap.GetD3dHeap(), samHeap.GetD3dHeap() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetGraphicsRootSignature(mGfxRootSignature.Get());
    mCommandList->SetComputeRootSignature(mComputeRootSignature.Get());

    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetComputeRootConstantBufferView(COMPUTE_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

    mExplosionParticleSystem->Update(
        gt,
        mAcceleration,
        mCommandList.Get(),
        mIndirectDispatch.Get(),
        psoLib["updateParticles"],
        psoLib["emitParticles"],
        psoLib["postUpdateParticles"],
        nullptr);


    mRainParticleSystem->Update(
        gt,
        mAcceleration,
        mCommandList.Get(),
        mIndirectDispatch.Get(),
        psoLib["updateParticles"],
        psoLib["emitParticles"],
        psoLib["postUpdateParticles"],
        mCurrFrameResource->RainParticleCountReadbackBuffer.Get());

    // Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
    // set as a root descriptor.
    auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
    mCommandList->SetGraphicsRootShaderResourceView(GFX_ROOT_ARG_MATERIAL_SRV, matBuffer->GetGPUVirtualAddress());

    DrawSceneToShadowMap();

    // TODO: Should execute command list here per pass?

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

	mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

    mCommandList->SetPipelineState(psoLib["opaque"]);
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    mTerrain->Draw(mCommandList.Get(), mIsWireframe ? psoLib["terrain_wireframe"] : psoLib["terrain"]);

    mCommandList->SetPipelineState(psoLib["debug"]);
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Debug]);

	mCommandList->SetPipelineState(psoLib["sky"]);
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);

    mExplosionParticleSystem->Draw(mCommandList.Get(), mIndirectDrawIndexed.Get(), psoLib["drawParticlesAddBlend"]);
    mRainParticleSystem->Draw(mCommandList.Get(), mIndirectDrawIndexed.Get(), psoLib["drawParticlesTransparencyBlend"]);

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

void TerrainApp::UpdateImgui(const GameTimer& gt)
{
    D3DApp::UpdateImgui(gt);

    // Define a panel to render GUI elements.
     
    ImGui::Begin("Options");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

    if(ImGui::CollapsingHeader("Features"))
    {
        ImGui::Checkbox("NormalMaps", &mNormalMapsEnabled);
        ImGui::Checkbox("Reflections", &mReflectionsEnabled);
        ImGui::Checkbox("Shadows", &mShadowsEnabled);
    }

    ImGui::Text("Rain particle count = %u", mDisplayedRainParticleCount);

    ImGui::SliderFloat("Rain emit rate", &mRainEmitRate, 1000.0f, 10000.0f);
    ImGui::SliderFloat("Rain scale", &mRainScale, 0.25f, 4.0f);
    ImGui::SliderFloat3("Acceleration", &mAcceleration.x, -20.0f, 20.0f);

    ImGui::Checkbox("Wireframe", &mIsWireframe);
    ImGui::Checkbox("Use TerrainHeightMap", &mUseTerrainHeightMap);
    ImGui::Checkbox("Use MaterialHeightMaps", &mUseMaterialHeightMaps);

    ImGui::SliderFloat("Min Tess Distance", &mMinTessDistance, 0.0f, 200.0f);
    ImGui::SliderFloat("Max Tess Distance", &mMaxTessDistance, 0.0f, 1000.0f);
    ImGui::SliderFloat("Max Tess", &mMaxTess, 0.0f, 6.0f);

    mTerrain->SetMaxTess(mMaxTess);
    mTerrain->SetMinTessDist(mMinTessDistance);
    mTerrain->SetMaxTessDist(mMaxTessDistance);

    mTerrain->SetUseTerrainHeightMap(mUseTerrainHeightMap);
    mTerrain->SetUseMaterialHeightMaps(mUseMaterialHeightMaps);


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

void TerrainApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        if((btnState & MK_RBUTTON) != 0)
        {
            if(mSpawnExplosion == false)
            {
                MathHelper::CalcPickingRay(
                    Vector2(static_cast<float>(x), static_cast<float>(y)),
                    Vector2(static_cast<float>(mClientWidth), static_cast<float>(mClientHeight)),
                    mCamera.GetView4x4f(), mCamera.GetProj4x4f(),
                    mWorldRayPos, mWorldRayDir);

                mSpawnExplosion = true;
            }
        }

        SetCapture(mhMainWnd);
    }
}

void TerrainApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void TerrainApp::OnMouseMove(WPARAM btnState, int x, int y)
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
 
void TerrainApp::OnKeyboardInput(const GameTimer& gt)
{
	const float dt = gt.DeltaTime();

    float cameraSpeed = 10.0f;
    if(GetAsyncKeyState(VK_SHIFT) & 0x8000)
        cameraSpeed = 100.0f;

	if(GetAsyncKeyState('W') & 0x8000)
		mCamera.Walk(cameraSpeed*dt);

	if(GetAsyncKeyState('S') & 0x8000)
		mCamera.Walk(-cameraSpeed*dt);

	if(GetAsyncKeyState('A') & 0x8000)
		mCamera.Strafe(-cameraSpeed*dt);

	if(GetAsyncKeyState('D') & 0x8000)
		mCamera.Strafe(cameraSpeed*dt);

	mCamera.UpdateViewMatrix();
}
 
void TerrainApp::AnimateMaterials(const GameTimer& gt)
{
	
}

void TerrainApp::UpdatePerObjectCB(const GameTimer& gt)
{
    // Update per object constants once per frame so the data can be shared across different render passes.
    for (auto& ri : mAllRitems)
    {
        XMStoreFloat4x4(&ri->ObjectConstants.gWorld, XMMatrixTranspose(XMLoadFloat4x4(&ri->World)));
        XMStoreFloat4x4(&ri->ObjectConstants.gTexTransform, XMMatrixTranspose(XMLoadFloat4x4(&ri->TexTransform)));
        ri->ObjectConstants.gMaterialIndex = ri->Mat->MatIndex;
        ri->ObjectConstants.gCubeMapIndex = mSkyBindlessIndex;

        // From documentation: 
        //   Make sure to keep the GraphicsResource handle alive as long as you need to access
        //   the memory on the CPU. For example, do not simply cache GpuAddress() and discard
        //   the GraphicsResource object, or your memory may be overwritten later.
        ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectConstants);
    }
}

void TerrainApp::UpdateMaterialBuffer(const GameTimer& gt)
{
    MaterialLib& matLib = MaterialLib::GetLib();

	auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
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
            matData.DisplacementScale = mat->DisplacementScale;
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

void TerrainApp::UpdateShadowTransform(const GameTimer& gt)
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

void TerrainApp::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX proj = mCamera.GetProj();

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    XMMATRIX shadowTransform = XMLoadFloat4x4(&mShadowTransform);

	XMStoreFloat4x4(&mMainPassCB.gView, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.gInvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.gProj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.gInvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.gViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.gInvViewProj, XMMatrixTranspose(invViewProj));
    XMStoreFloat4x4(&mMainPassCB.gShadowTransform, XMMatrixTranspose(shadowTransform));

    MathHelper::ExtractFrustumPlanes(viewProj, mMainPassCB.gWorldFrustumPlanes);

	mMainPassCB.gEyePosW = mCamera.GetPosition3f();
	mMainPassCB.gRenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.gInvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.gNearZ = mCamera.GetNearZ();
	mMainPassCB.gFarZ = mCamera.GetFarZ();
	mMainPassCB.gTotalTime = gt.TotalTime();
	mMainPassCB.gDeltaTime = gt.DeltaTime();
	mMainPassCB.gAmbientLight = { 0.15f, 0.15f, 0.25f, 1.0f };
    mMainPassCB.gSkyBoxIndex = mSkyBindlessIndex;
    mMainPassCB.gRandomTexIndex = mRandomTexBindlessIndex;
    mMainPassCB.gSunShadowMapIndex = mShadowMapBindlessIndex;

    mMainPassCB.gNormalMapsEnabled = mNormalMapsEnabled;
    mMainPassCB.gReflectionsEnabled = mReflectionsEnabled;
    mMainPassCB.gShadowsEnabled = mShadowsEnabled;
    mMainPassCB.gSsaoEnabled = false;

    mMainPassCB.gNumDirLights = 3;
    mMainPassCB.gNumPointLights = 0;
    mMainPassCB.gNumSpotLights = 0;

	mMainPassCB.gLights[0].Direction = mRotatedLightDirections[0];
	mMainPassCB.gLights[0].Strength = { 0.8f, 0.8f, 0.8f };
	mMainPassCB.gLights[1].Direction = mRotatedLightDirections[1];
	mMainPassCB.gLights[1].Strength = { 0.1f, 0.1f, 0.1f };
	mMainPassCB.gLights[2].Direction = mRotatedLightDirections[2];
	mMainPassCB.gLights[2].Strength = { 0.1f, 0.1f, 0.1f };
 
	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void TerrainApp::UpdateShadowPassCB(const GameTimer& gt)
{
    XMMATRIX view = XMLoadFloat4x4(&mLightView);
    XMMATRIX proj = XMLoadFloat4x4(&mLightProj);

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    UINT w = mShadowMap->Width();
    UINT h = mShadowMap->Height();

    XMStoreFloat4x4(&mShadowPassCB.gView, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mShadowPassCB.gInvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&mShadowPassCB.gProj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mShadowPassCB.gInvProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&mShadowPassCB.gViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mShadowPassCB.gInvViewProj, XMMatrixTranspose(invViewProj));

    MathHelper::ExtractFrustumPlanes(viewProj, mShadowPassCB.gWorldFrustumPlanes);

    mShadowPassCB.gEyePosW = mCamera.GetPosition3f();
    mShadowPassCB.gRenderTargetSize = XMFLOAT2((float)w, (float)h);
    mShadowPassCB.gInvRenderTargetSize = XMFLOAT2(1.0f / w, 1.0f / h);
    mShadowPassCB.gNearZ = mLightNearZ;
    mShadowPassCB.gFarZ = mLightFarZ;

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(1, mShadowPassCB);
}

void TerrainApp::EmitExplosionParticles(const GameTimer& gt)
{
    TextureLib& texLib = TextureLib::GetLib();

    Vector3 spawnPos = mWorldRayPos + mWorldRayDir * MathHelper::RandF(5.0f, 20.0f);

    uint32_t numParticlesEmitted = MathHelper::Rand(2000, 3000);

    if (mSpawnExplosion)
    {
        ParticleEmitCB explosionParticles;
        explosionParticles.gEmitBoxMin = spawnPos + Vector3(-0.1f, -0.1f, -0.1f);
        explosionParticles.gEmitBoxMax = spawnPos + Vector3(+0.1f, 0.1f, +0.1f);

        explosionParticles.gMinLifetime = 0.3f;
        explosionParticles.gMaxLifetime = 0.9f;

        explosionParticles.gEmitDirectionMin = Vector3(-1.0f, -1.0f, -1.0f);
        explosionParticles.gEmitDirectionMax = Vector3(+1.0f, +1.0f, +1.0f);

        explosionParticles.gEmitColorMin = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        explosionParticles.gEmitColorMax = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

        explosionParticles.gMinInitialSpeed = 20.0f;
        explosionParticles.gMaxInitialSpeed = 80.0f;

        explosionParticles.gMinRotation = 0.0f;
        explosionParticles.gMaxRotation = 2.0f*MathHelper::Pi;
        explosionParticles.gMinRotationSpeed = 1.0f;
        explosionParticles.gMaxRotationSpeed = 5.0f;

        explosionParticles.gMinScale = Vector2(0.5f);
        explosionParticles.gMaxScale = Vector2(1.5f);

        explosionParticles.gDragScale = 0.75f;
        explosionParticles.gEmitCount = numParticlesEmitted;
        explosionParticles.gBindlessTextureIndex = texLib["explosionParticle"]->BindlessIndex;

        explosionParticles.gEmitRandomValues.x = MathHelper::RandF();
        explosionParticles.gEmitRandomValues.y = MathHelper::RandF();
        explosionParticles.gEmitRandomValues.z = MathHelper::RandF();
        explosionParticles.gEmitRandomValues.w = MathHelper::RandF();

        mExplosionParticleSystem->Emit(explosionParticles);

        mSpawnExplosion = false;
    }
}

void TerrainApp::EmitRainParticles(const GameTimer& gt)
{
    TextureLib& texLib = TextureLib::GetLib();

    Vector3 camPos = mCamera.GetPosition();

    static float rainParticlesToEmit = 0.0f;
    rainParticlesToEmit += gt.DeltaTime() * mRainEmitRate;

    // Wait until we have enough particles to fill one thread group.
    if(rainParticlesToEmit > 128.0f)
    {
        uint32_t numParticlesEmitted = static_cast<uint32_t>(rainParticlesToEmit);

        ParticleEmitCB rainParticles;
        rainParticles.gEmitBoxMin = camPos + Vector3(-40.0f, 8.0f, -40.0f);
        rainParticles.gEmitBoxMax = camPos + Vector3(+40.0f, 10.0f, +40.0f);

        rainParticles.gMinLifetime = 2.5f;
        rainParticles.gMaxLifetime = 3.5f;

        rainParticles.gEmitDirectionMin = Vector3(-1.0f, -4.0f, -1.0f);
        rainParticles.gEmitDirectionMax = Vector3(+1.0f, -3.0f, +1.0f);

        rainParticles.gEmitColorMin = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        rainParticles.gEmitColorMax = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

        rainParticles.gMinInitialSpeed = 0.0f;
        rainParticles.gMaxInitialSpeed = 0.0f;

        rainParticles.gMinRotation = 0.0f;
        rainParticles.gMaxRotation = 0.0f;
        rainParticles.gMinRotationSpeed = 0.0f;
        rainParticles.gMaxRotationSpeed = 0.0f;

        rainParticles.gMinScale = mRainScale * Vector2(0.1f, 0.2f);
        rainParticles.gMaxScale = mRainScale * Vector2(0.2f, 0.3f);

        rainParticles.gDragScale = 0.0f;
        rainParticles.gEmitCount = numParticlesEmitted;
        rainParticles.gBindlessTextureIndex = texLib["rainParticle"]->BindlessIndex;

        rainParticles.gEmitRandomValues.x = MathHelper::RandF();
        rainParticles.gEmitRandomValues.y = MathHelper::RandF();
        rainParticles.gEmitRandomValues.z = MathHelper::RandF();
        rainParticles.gEmitRandomValues.w = MathHelper::RandF();

        mRainParticleSystem->Emit(rainParticles);

        rainParticlesToEmit -= numParticlesEmitted;
    }
}

void TerrainApp::ReadParticleCounts(const GameTimer& gt)
{
    uint32_t* rainParticleCount = nullptr;
    ThrowIfFailed(mCurrFrameResource->RainParticleCountReadbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&rainParticleCount)));

    // This is actually gNumFrameResources frames behind so we do not have to block GPU to read.
    mRainParticleCount = rainParticleCount[0];

    mCurrFrameResource->RainParticleCountReadbackBuffer->Unmap(0, nullptr);
    
    static float particleCountPollTime = 0.0f;
    particleCountPollTime += gt.DeltaTime();

    // For display, do not update every frame as it is hard to read text changing that fast :)
    if(particleCountPollTime >= 0.5f)
    {
        mDisplayedRainParticleCount = mRainParticleCount;
        particleCountPollTime -= 0.5f;

        if(mDisplayedRainParticleCount == 0)
            MessageBox(0, 0, 0, 0);
    }
}

void TerrainApp::LoadTextures()
{
    TextureLib& texLib = TextureLib::GetLib();
    texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
}

void TerrainApp::LoadGeometry()
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

void TerrainApp::BuildRootSignatures()
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

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&gfxRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if(errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mGfxRootSignature.GetAddressOf())));


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

void TerrainApp::BuildCommandSignatures()
{
    // Since the particle system is updated on the GPU, we do not know how many particles are 
    // alive on the CPU. Thus we do not know how many particles to draw. Reading from GPU memory to
    // CPU memory is slow. The DrawIndirect API allows us to specify the draw arguments via a GPU 
    // buffer, so we can keep everything on the GPU.

    // Describe the data of each indirect argument. The order here must match the actual data.
    // This can be more complicated to set root constants and change vertex buffer views, for example.
    D3D12_INDIRECT_ARGUMENT_DESC indirectDispatchArgs[1];
    indirectDispatchArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC indirectDispatchDesc;
    indirectDispatchDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    indirectDispatchDesc.NumArgumentDescs = 1;
    indirectDispatchDesc.pArgumentDescs = indirectDispatchArgs;
    indirectDispatchDesc.NodeMask = 0; // used for multiple GPUs

    ThrowIfFailed(md3dDevice->CreateCommandSignature(
        &indirectDispatchDesc,
        nullptr, // root args not changing
        IID_PPV_ARGS(mIndirectDispatch.GetAddressOf())));

    D3D12_INDIRECT_ARGUMENT_DESC indirectDrawIndexedArgs[1];
    indirectDrawIndexedArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC indirectDrawIndexedDesc;
    indirectDrawIndexedDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    indirectDrawIndexedDesc.NumArgumentDescs = 1;
    indirectDrawIndexedDesc.pArgumentDescs = indirectDrawIndexedArgs;
    indirectDrawIndexedDesc.NodeMask = 0; // used for multiple GPUs

    ThrowIfFailed(md3dDevice->CreateCommandSignature(
        &indirectDrawIndexedDesc,
        nullptr, // root args not changing
        IID_PPV_ARGS(mIndirectDrawIndexed.GetAddressOf())));
}

void TerrainApp::BuildCbvSrvUavDescriptorHeap()
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

	//
	// Fill out the heap with actual descriptors.
	//

    InitImgui(cbvSrvUavHeap);

    mShadowMapBindlessIndex = mShadowMap->BuildDescriptors(mDsvHeap.CpuHandle(DSV_SHADOWMAP));

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

    mExplosionParticleSystem->BuildDescriptors();
    mRainParticleSystem->BuildDescriptors();

    mTerrain->BuildDescriptors();
}

void TerrainApp::BuildShaders()
{
    ShaderLib::GetLib().Init(md3dDevice.Get());
}

void TerrainApp::BuildPSOs()
{
    PsoLib::GetLib().Init(
        md3dDevice.Get(),
        mBackBufferFormat,
        mDepthStencilFormat,
        SsaoAmbientMapFormat,
        SceneNormalMapFormat,
        mGfxRootSignature.Get(),
        mComputeRootSignature.Get());
}

void TerrainApp::BuildFrameResources()
{
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            2, (UINT)mAllRitems.size(), MaterialLib::GetLib().GetMaterialCount()));
    }
}

void TerrainApp::BuildMaterials()
{
    MaterialLib::GetLib().Init(md3dDevice.Get());
}

void TerrainApp::AddRenderItem(RenderLayer layer, const XMFLOAT4X4& world, const XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs)
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

void TerrainApp::BuildRenderItems()
{
    MaterialLib& matLib = MaterialLib::GetLib();

    XMFLOAT4X4 worldTransform;
    XMFLOAT4X4 texTransform;

    XMMATRIX columnSceneOffset = XMMatrixTranslation(5.0f, -33.0f, -160.0f);

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Sky, worldTransform, texTransform, matLib["sky"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
    
    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(6.0f, 6.0f, 6.0f) * columnSceneOffset);
    XMStoreFloat4x4(&texTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["orbBase"], mGeometries["orbBase"].get(), mGeometries["orbBase"]->DrawArgs["subset0"]);

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(0.0f, 1.75f, 0.0f) * columnSceneOffset);
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror1"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

    worldTransform = MathHelper::Identity4x4() * columnSceneOffset;
    XMStoreFloat4x4(&texTransform, XMMatrixScaling(6.0f, 6.0f, 1.0f) );
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["stoneFloor"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"]);

    XMMATRIX falledColumnTransform0 =
        XMMatrixRotationZ(-0.54f * XM_PI) *
        XMMatrixRotationY(0.15f * XM_PI) *
        XMMatrixScaling(1.0f, 1.0f, 1.0f) *
        XMMatrixTranslation(-3.0f, 0.35f, 3.0f) * columnSceneOffset;
    XMStoreFloat4x4(&worldTransform, falledColumnTransform0);
    texTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnRound"], mGeometries["columnRoundBroken"].get(), mGeometries["columnRoundBroken"]->DrawArgs["subset0"]);

    XMMATRIX falledColumnTransform1 =
        XMMatrixRotationZ(-0.54f * XM_PI) *
        XMMatrixRotationY(0.75f * XM_PI) *
        XMMatrixScaling(1.0f, 1.0f, 1.0f) *
        XMMatrixTranslation(1.5f, 0.35f, -4.0f) * columnSceneOffset;
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
        XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(-5.0f, 0.0f, -10.0f + i * 5.0f) * columnSceneOffset);
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameLeft].get(), mGeometries[columnNameLeft]->DrawArgs["subset0"]);

        XMStoreFloat4x4(&texTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
        XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(+5.0f, 0.0f, -10.0f + i * 5.0f) * columnSceneOffset);
        AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["columnSquare"], mGeometries[columnNameRight].get(), mGeometries[columnNameRight]->DrawArgs["subset0"]);

        if(!isLeftColumnBroken)
        {
            texTransform = MathHelper::Identity4x4();
            XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(-5.0f, 4.0f, -10.0f + i * 5.0f) * columnSceneOffset);
            AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
        }

        if(!isRightColumnBroken)
        {
            texTransform = MathHelper::Identity4x4();
            XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(+5.0f, 4.0f, -10.0f + i * 5.0f) * columnSceneOffset);
            AddRenderItem(RenderLayer::Opaque, worldTransform, texTransform, matLib["mirror0"], mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
        }
    }
}

void TerrainApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
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

void TerrainApp::DrawSceneToShadowMap()
{
    PsoLib& psoLib = PsoLib::GetLib();

    mCommandList->RSSetViewports(1, &mShadowMap->Viewport());
    mCommandList->RSSetScissorRects(1, &mShadowMap->ScissorRect());

    // Change to DEPTH_WRITE.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

    UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerPassCB));

    // Clear the back buffer and depth buffer.
    mCommandList->ClearDepthStencilView(mShadowMap->Dsv(), 
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Set null render target because we are only going to draw to
    // depth buffer.  Setting a null render target will disable color writes.
    // Note the active PSO also must specify a render target count of 0.
    mCommandList->OMSetRenderTargets(0, nullptr, false, &mShadowMap->Dsv());

    // Bind the pass constant buffer for the shadow map pass.
    auto passCB = mCurrFrameResource->PassCB->Resource();
    D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = passCB->GetGPUVirtualAddress() + 1*passCBByteSize;
    mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCBAddress);

    mCommandList->SetPipelineState(psoLib["shadow_opaque"]);

    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    mTerrain->Draw(mCommandList.Get(), psoLib["terrain_shadow"]);

    // Change back to GENERIC_READ so we can read the texture in a shader.
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));
}



