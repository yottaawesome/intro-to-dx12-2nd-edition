//***************************************************************************************
// RayTracingIntroApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#include "RayTracingIntroApp.h"

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
        RayTracingIntroApp theApp(hInstance);
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

RayTracingIntroApp::RayTracingIntroApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
    // Estimate the scene bounding sphere manually since we know how the scene was constructed.
    // In general, you need to loop over every world space vertex position and compute the bounding sphere.
    mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
    mSceneBounds.Radius = 512; 
}

RayTracingIntroApp::~RayTracingIntroApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool RayTracingIntroApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

	mCamera.SetPosition(0.0f, 3.0f, -20.0f);
 
    // Create the singleton.
    // GraphicsMemory::Get(md3dDevice.Get());

    // We will upload on the direct queue for the book samples, but 
    // copy queue would be better for real game.
    mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

    // Do init work that requires mUploadBatch...
	LoadTextures();



    // Kick off upload work asyncronously.
    std::future<void> result = mUploadBatch->End(mCommandQueue.Get());

    // Other init work while uploading.
    BuildRootSignatures();
	BuildCbvSrvUavDescriptorHeap();
    BuildMaterials();
    BuildShaders();
    BuildRayTraceScene(); //BuildRenderItems();
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

void RayTracingIntroApp::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2);
}
 
void RayTracingIntroApp::OnResize()
{
    D3DApp::OnResize();

	mCamera.SetLens(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1500.0f);

    if(mRayTracer != nullptr)
    {
        mRayTracer->OnResize(mClientWidth, mClientHeight);
    }
}

void RayTracingIntroApp::Update(const GameTimer& gt)
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
}

void RayTracingIntroApp::Draw(const GameTimer& gt)
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
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), nullptr));

    // SetDescriptorHeaps must be called before SetGraphicsRootSignature when using HEAP_DIRECTLY_INDEXED.
    ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavHeap.GetD3dHeap(), samHeap.GetD3dHeap() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetGraphicsRootSignature(mGfxRootSignature.Get());
    mCommandList->SetComputeRootSignature(mComputeRootSignature.Get());

    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetComputeRootConstantBufferView(COMPUTE_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

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

	mCommandList->SetGraphicsRootConstantBufferView(GFX_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

    mCommandList->SetPipelineState(psoLib["opaque"]);
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    mCommandList->SetPipelineState(psoLib["debug"]);
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Debug]);

	mCommandList->SetPipelineState(psoLib["sky"]);
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);

    if(mRayTracer != nullptr)
    {
        mRayTracer->Draw(passCB, matBuffer);

        ID3D12Resource* renderTarget = CurrentBackBuffer();
        ID3D12Resource* rayTraceOutput = mRayTracer->GetOutputImage();

        D3D12_RESOURCE_BARRIER preCopyBarriers[2];
        preCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        preCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(rayTraceOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        mCommandList->ResourceBarrier(ARRAYSIZE(preCopyBarriers), preCopyBarriers);

        mCommandList->CopyResource(renderTarget, rayTraceOutput);

        D3D12_RESOURCE_BARRIER postCopyBarriers[2];
        postCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        postCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(rayTraceOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        mCommandList->ResourceBarrier(ARRAYSIZE(postCopyBarriers), postCopyBarriers);

        // Restore 
        mCommandList->SetComputeRootSignature(mComputeRootSignature.Get());
        mCommandList->SetComputeRootConstantBufferView(COMPUTE_ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());
    }

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

void RayTracingIntroApp::UpdateImgui(const GameTimer& gt)
{
    D3DApp::UpdateImgui(gt);

    // Define a panel to render GUI elements.
     
    ImGui::Begin("Options");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

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

void RayTracingIntroApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        SetCapture(mhMainWnd);
    }
}

void RayTracingIntroApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void RayTracingIntroApp::OnMouseMove(WPARAM btnState, int x, int y)
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
 
void RayTracingIntroApp::OnKeyboardInput(const GameTimer& gt)
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
 
void RayTracingIntroApp::AnimateMaterials(const GameTimer& gt)
{
	
}

void RayTracingIntroApp::UpdatePerObjectCB(const GameTimer& gt)
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

void RayTracingIntroApp::UpdateMaterialBuffer(const GameTimer& gt)
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
            matData.DisplacementScale = mat->DisplacementScale;
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

void RayTracingIntroApp::UpdateShadowTransform(const GameTimer& gt)
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

void RayTracingIntroApp::UpdateMainPassCB(const GameTimer& gt)
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
	mMainPassCB.gAmbientLight = { 0.35f, 0.35f, 0.45f, 1.0f };
    mMainPassCB.gSkyBoxIndex = mSkyBindlessIndex;
    mMainPassCB.gRandomTexIndex = mRandomTexBindlessIndex;
    mMainPassCB.gSunShadowMapIndex = mShadowMapBindlessIndex;
    mMainPassCB.gRayTraceImageIndex = mRayTracer->GetOutputTextureUavIndex();
    mMainPassCB.gDebugTexIndex = mShadowMapBindlessIndex;
	mMainPassCB.gLights[0].Direction = mRotatedLightDirections[0];
	mMainPassCB.gLights[0].Strength = { 0.8f, 0.8f, 0.8f };
	mMainPassCB.gLights[1].Direction = mRotatedLightDirections[1];
	mMainPassCB.gLights[1].Strength = { 0.1f, 0.1f, 0.1f };
	mMainPassCB.gLights[2].Direction = mRotatedLightDirections[2];
	mMainPassCB.gLights[2].Strength = { 0.1f, 0.1f, 0.1f };
 
	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void RayTracingIntroApp::LoadTextures()
{
    TextureLib& texLib = TextureLib::GetLib();
    texLib.Init(md3dDevice.Get(), *mUploadBatch.get());
}

void RayTracingIntroApp::BuildRootSignatures()
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

void RayTracingIntroApp::BuildCbvSrvUavDescriptorHeap()
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
}

void RayTracingIntroApp::BuildShaders()
{
    ShaderLib::GetLib().Init(md3dDevice.Get());
}

void RayTracingIntroApp::BuildPSOs()
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

void RayTracingIntroApp::BuildFrameResources()
{
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            2, (UINT)mAllRitems.size(), MaterialLib::GetLib().GetMaterialCount()));
    }
}

void RayTracingIntroApp::BuildMaterials()
{
    MaterialLib::GetLib().Init(md3dDevice.Get());
}

void RayTracingIntroApp::BuildRayTraceScene()
{
    ShaderLib& shaderLib = ShaderLib::GetLib();
    MaterialLib& matLib = MaterialLib::GetLib();

    mRayTracer = std::make_unique<ProceduralRayTracer>(
        md3dDevice.Get(), 
        mCommandList.Get(), 
        shaderLib["rayTracingLib"],
        DXGI_FORMAT_R8G8B8A8_UNORM, 
        mClientWidth, mClientHeight);

    // Basically ray tracing variation of BuildRenderItems.
    //   The box is [-1, 1]^3 in local space.
    //   void AddBox(const DirectX::XMFLOAT4X4& worldTransform, UINT materialIndex);
    // 
    //   The cylinder is centered at the origin, aligned with +y axis, has radius 1 and length 2 in local space.
    //   void AddCylinder(const DirectX::XMFLOAT4X4& worldTransform, UINT materialIndex);
    // 
    //   The sphere is centered at origin with radius 1 in local space.
    //   void AddSphere(const DirectX::XMFLOAT4X4& worldTransform, UINT materialIndex);

    XMFLOAT4X4 worldTransform;

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(2.0f, 1.0f, 2.0f) * XMMatrixTranslation(0.0f, 1.0f, 0.0f));
    mRayTracer->AddBox(worldTransform, XMFLOAT2(1.0f, 0.5f), matLib["bricks0"]->MatIndex);

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(20.0f, 0.01f, 30.0f));
    mRayTracer->AddBox(worldTransform, XMFLOAT2(8.0f, 8.0f), matLib["tile0"]->MatIndex);

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(1.5f, 1.5f, 1.5f) * XMMatrixTranslation(0.0f, 3.5f, 0.0f));
    mRayTracer->AddSphere(worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["glass0"]->MatIndex);

    for(int i = 0; i < 5; ++i)
    {
        XMStoreFloat4x4(&worldTransform, XMMatrixScaling(0.5f, 1.5f, 0.5f) * XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f));
        mRayTracer->AddCylinder(worldTransform, XMFLOAT2(1.5f, 2.0f), matLib["bricks0"]->MatIndex);

        XMStoreFloat4x4(&worldTransform, XMMatrixScaling(0.5f, 1.5f, 0.5f) * XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f));
        mRayTracer->AddCylinder(worldTransform, XMFLOAT2(1.5f, 2.0f), matLib["bricks0"]->MatIndex);

        XMStoreFloat4x4(&worldTransform, XMMatrixScaling(0.5f, 1.0f, 0.5f) * XMMatrixTranslation(-5.0f, 3.0f, -10.0f + i * 5.0f));
        mRayTracer->AddDisk(worldTransform, XMFLOAT2(0.25f, 0.25f), matLib["bricks0"]->MatIndex);

        XMStoreFloat4x4(&worldTransform, XMMatrixScaling(0.5f, 1.0f, 0.5f) * XMMatrixTranslation(+5.0f, 3.0f, -10.0f + i * 5.0f));
        mRayTracer->AddDisk(worldTransform, XMFLOAT2(0.25f, 0.25f), matLib["bricks0"]->MatIndex);

        XMStoreFloat4x4(&worldTransform, XMMatrixScaling(0.5f, 0.5f, 0.5f) * XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f));
        mRayTracer->AddSphere(worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["mirror0"]->MatIndex);

        XMStoreFloat4x4(&worldTransform, XMMatrixScaling(0.5f, 0.5f, 0.5f) * XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f));
        mRayTracer->AddSphere(worldTransform, XMFLOAT2(1.0f, 1.0f), matLib["mirror0"]->MatIndex);
    }
}

void RayTracingIntroApp::AddRenderItem(RenderLayer layer, const XMFLOAT4X4& world, const XMFLOAT4X4& texTransform, Material* mat, MeshGeometry* geo, SubmeshGeometry& drawArgs)
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

void RayTracingIntroApp::BuildRenderItems()
{
    
}

void RayTracingIntroApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
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




