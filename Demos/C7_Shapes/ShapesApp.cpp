
#include "ShapesApp.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

constexpr UINT CBV_SRV_UAV_HEAP_CAPACITY = 16384;

struct ColorVertex
{
    XMFLOAT3 Pos;
    XMFLOAT4 Color;
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
        ShapesApp theApp(hInstance);
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

ShapesApp::ShapesApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{

}

ShapesApp::~ShapesApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool ShapesApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

    // We will upload on the direct queue for the book samples, but 
    // copy queue would be better for real game.
    mUploadBatch->Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);

    std::unique_ptr<MeshGeometry> shapeGeo = BuildShapeGeometry(
        md3dDevice.Get(), *mUploadBatch.get());
    if(shapeGeo != nullptr)
    {
        mGeometries[shapeGeo->Name] = std::move(shapeGeo);
    }

    // Kick off upload work asyncronously.
    std::future<void> result = mUploadBatch->End(mCommandQueue.Get());

    // Other init work...
    BuildRootSignature();
    BuildCbvSrvUavDescriptorHeap();
    BuildShadersAndInputLayout();
    BuildRenderItems();
    BuildFrameResources();
    BuildPSOs();

    // Block until the upload work is complete.
    result.wait();

    return true;
}

void ShapesApp::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
}

void ShapesApp::OnResize()
{
    D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);
}

void ShapesApp::Update(const GameTimer& gt)
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
        HANDLE eventHandle = CreateEventEx(
            nullptr, nullptr,
            false, EVENT_ALL_ACCESS);

        ThrowIfFailed(mFence->SetEventOnCompletion(
            mCurrFrameResource->Fence,
            eventHandle));

        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    UpdatePerObjectCB(gt);
    UpdateMainPassCB(gt);
}

void ShapesApp::Draw(const GameTimer& gt)
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();

    UpdateImgui(gt);

    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(cmdListAlloc->Reset());

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

    ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavHeap.GetD3dHeap() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    // Indicate a state transition on the resource usage.
    mCommandList->ResourceBarrier(
        1, &CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, 
        D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear the back buffer and depth buffer.
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    ID3D12Resource* passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetGraphicsRootConstantBufferView(
        ROOT_ARG_PASS_CBV, passCB->GetGPUVirtualAddress());

    mCommandList->SetPipelineState(
        mDrawWireframe ?
        mPSOs["opaque_wireframe"].Get() :
        mPSOs["opaque"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    // Draw imgui UI.
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

    // Indicate a state transition on the resource usage.
    mCommandList->ResourceBarrier(
        1, &CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT));

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

void ShapesApp::UpdateImgui(const GameTimer& gt)
{
    D3DApp::UpdateImgui(gt);

    //
    // Define a panel to render GUI elements.
    // 
    ImGui::Begin("Options");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

    ImGui::Checkbox("Wireframe", &mDrawWireframe);

    GraphicsMemoryStatistics gfxMemStats = GraphicsMemory::Get(md3dDevice.Get()).GetStatistics();

    if(ImGui::CollapsingHeader("VideoMemoryInfo"))
    {
        static float vidMemPollTime = 0.0f;
        vidMemPollTime += gt.DeltaTime();

        static DXGI_QUERY_VIDEO_MEMORY_INFO videoMemInfo;
        if(vidMemPollTime >= 1.0f) // poll every second
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
    if(ImGui::CollapsingHeader("GraphicsMemoryStatistics"))
    {
        ImGui::Text("Bytes of memory in-flight: %u", gfxMemStats.committedMemory);
        ImGui::Text("Total bytes used: %u", gfxMemStats.totalMemory);
        ImGui::Text("Total page count: %u", gfxMemStats.totalPages);
    }

    ImGui::End();

    ImGui::Render();
}

void ShapesApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if(!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        SetCapture(mhMainWnd);
    }
}

void ShapesApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if(!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void ShapesApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if(!io.WantCaptureMouse)
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
            mRadius = MathHelper::Clamp(mRadius, 5.0f, 25.0f);
        }

        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }
}

void ShapesApp::OnKeyboardInput(const GameTimer& gt)
{
}

void ShapesApp::UpdateCamera(const GameTimer& gt)
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

void ShapesApp::UpdatePerObjectCB(const GameTimer& gt)
{
    // Update per object constants once per frame so the data can be shared across different render passes.
    for(auto& ri : mAllRitems)
    {
        XMStoreFloat4x4(
            &ri->ObjectCB.World,
            XMMatrixTranspose(XMLoadFloat4x4(&ri->World)));

        // Need to hold handle until we submit work to GPU.
        ri->MemHandleToObjectCB = mLinearAllocator->AllocateConstant(ri->ObjectCB);
    }
}

void ShapesApp::UpdateMainPassCB(const GameTimer& gt)
{
    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);
}

void ShapesApp::BuildCbvSrvUavDescriptorHeap()
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

    InitImgui(cbvSrvUavHeap);
}

void ShapesApp::BuildRootSignature()
{
    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER gfxRootParameters[ROOT_ARG_COUNT];

    // Perfomance TIP: Order from most frequent to least frequent.
    gfxRootParameters[ROOT_ARG_OBJECT_CBV].InitAsConstantBufferView(0);
    gfxRootParameters[ROOT_ARG_PASS_CBV].InitAsConstantBufferView(1);

    // A root signature is an array of root parameters.
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        ROOT_ARG_COUNT,
        gfxRootParameters,
        0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

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

void ShapesApp::BuildShadersAndInputLayout()
{
#if defined(DEBUG) || defined(_DEBUG)  
#define COMMA_DEBUG_ARGS ,DXC_ARG_DEBUG, DXC_ARG_SKIP_OPTIMIZATIONS
#else
#define COMMA_DEBUG_ARGS
#endif

    std::vector<LPCWSTR> vsArgs = std::vector<LPCWSTR> { L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> psArgs = std::vector<LPCWSTR> { L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

    mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", vsArgs);
    mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\BasicColor.hlsl", psArgs);

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
}

void ShapesApp::BuildPSOs()
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
}

void ShapesApp::BuildFrameResources()
{
    constexpr UINT passCount = 1;
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(
            std::make_unique<FrameResource>(md3dDevice.Get(),
            passCount));
    }
}

void ShapesApp::AddRenderItem(RenderLayer layer, const DirectX::XMFLOAT4X4& world, MeshGeometry* geo, SubmeshGeometry& drawArgs)
{
    auto ritem = std::make_unique<RenderItem>();
    ritem->World = world;
    ritem->TexTransform = MathHelper::Identity4x4();
    ritem->Mat = nullptr;
    ritem->Geo = geo;
    ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    ritem->IndexCount = drawArgs.IndexCount;
    ritem->StartIndexLocation = drawArgs.StartIndexLocation;
    ritem->BaseVertexLocation = drawArgs.BaseVertexLocation;

    mRitemLayer[(int)layer].push_back(ritem.get());
    mAllRitems.push_back(std::move(ritem));
}

void ShapesApp::BuildRenderItems()
{
    XMFLOAT4X4 worldTransform;

    XMStoreFloat4x4(&worldTransform, XMMatrixScaling(2.0f, 1.0f, 2.0f) * XMMatrixTranslation(0.0f, 0.5f, 0.0f));
    AddRenderItem(RenderLayer::Opaque, worldTransform, mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["box"]);

    worldTransform = MathHelper::Identity4x4();
    AddRenderItem(RenderLayer::Opaque, worldTransform, mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["grid"]);

    for(int i = 0; i < 5; ++i)
    {
        XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["cylinder"]);

        XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["cylinder"]);

        XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);

        XMStoreFloat4x4(&worldTransform, XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f));
        AddRenderItem(RenderLayer::Opaque, worldTransform, mGeometries["shapeGeo"].get(), mGeometries["shapeGeo"]->DrawArgs["sphere"]);
    }
}

void ShapesApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    for(size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        cmdList->SetGraphicsRootConstantBufferView(ROOT_ARG_OBJECT_CBV, ri->MemHandleToObjectCB.GpuAddress());

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}

std::unique_ptr<MeshGeometry> ShapesApp::BuildShapeGeometry(
    ID3D12Device* device,
    ResourceUploadBatch& uploadBatch)
{
    MeshGen meshGen;
    MeshGenData box = meshGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
    MeshGenData grid = meshGen.CreateGrid(20.0f, 30.0f, 60, 40);
    MeshGenData sphere = meshGen.CreateSphere(0.5f, 20, 20);
    MeshGenData cylinder = meshGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);
    MeshGenData quad = meshGen.CreateQuad(0.0f, 0.0f, 1.0f, 1.0f, 0.0f);

    //
    // We are concatenating all the geometry into one big vertex/index buffer.  So
    // define the regions in the buffer each submesh covers.
    //
    MeshGenData compositeMesh;
    SubmeshGeometry boxSubmesh = compositeMesh.AppendSubmesh(box);
    SubmeshGeometry gridSubmesh = compositeMesh.AppendSubmesh(grid);
    SubmeshGeometry sphereSubmesh = compositeMesh.AppendSubmesh(sphere);
    SubmeshGeometry cylinderSubmesh = compositeMesh.AppendSubmesh(cylinder);
    SubmeshGeometry quadSubmesh = compositeMesh.AppendSubmesh(quad);

    XMFLOAT4 color = XMFLOAT4(0.2f, 0.2f, 0.2f, 1.0f);

    // Extract the vertex elements we are interested into our vertex buffer. 
    std::vector<ColorVertex> vertices(compositeMesh.Vertices.size());
    for(size_t i = 0; i < compositeMesh.Vertices.size(); ++i)
    {
        vertices[i].Pos = compositeMesh.Vertices[i].Position;
        vertices[i].Color = color;
    }

    const uint32_t indexCount = (UINT)compositeMesh.Indices32.size();

    const UINT indexElementByteSize = sizeof(uint16_t);
    const UINT vbByteSize = (UINT)vertices.size() * sizeof(ColorVertex);
    const UINT ibByteSize = indexCount * indexElementByteSize;

    const byte* indexData = reinterpret_cast<byte*>(compositeMesh.GetIndices16().data());

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "shapeGeo";

    geo->VertexBufferCPU.resize(vbByteSize);
    CopyMemory(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

    geo->IndexBufferCPU.resize(ibByteSize);
    CopyMemory(geo->IndexBufferCPU.data(), indexData, ibByteSize);

    CreateStaticBuffer(
        device, uploadBatch,
        vertices.data(), vertices.size(), sizeof(ColorVertex),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, 
        &geo->VertexBufferGPU);

    CreateStaticBuffer(
        device, uploadBatch,
        indexData, indexCount, indexElementByteSize,
        D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);

    geo->VertexByteStride = sizeof(ColorVertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    geo->DrawArgs["box"] = boxSubmesh;
    geo->DrawArgs["grid"] = gridSubmesh;
    geo->DrawArgs["sphere"] = sphereSubmesh;
    geo->DrawArgs["cylinder"] = cylinderSubmesh;
    geo->DrawArgs["quad"] = quadSubmesh;

    return geo;
}