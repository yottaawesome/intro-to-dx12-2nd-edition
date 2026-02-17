
#include "InitDirect3DApp.h"

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
        InitDirect3DApp theApp(hInstance);
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

InitDirect3DApp::InitDirect3DApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{

}

InitDirect3DApp::~InitDirect3DApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool InitDirect3DApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

    BuildCbvSrvUavDescriptorHeap();

    return true;
}

void InitDirect3DApp::CreateRtvAndDsvDescriptorHeaps()
{
    mRtvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SwapChainBufferCount);
    mDsvHeap.Init(md3dDevice.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, SwapChainBufferCount);
}
 
void InitDirect3DApp::OnResize()
{
    D3DApp::OnResize();
}

void InitDirect3DApp::Update(const GameTimer& gt)
{
   
}

void InitDirect3DApp::Draw(const GameTimer& gt)
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();

    UpdateImgui(gt);

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(mDirectCmdListAlloc->Reset());

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavHeap.GetD3dHeap() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear the back buffer and depth buffer.
    mCommandList->ClearRenderTargetView(
        CurrentBackBufferView(), 
        Colors::LightSteelBlue, 
        0, nullptr);
    mCommandList->ClearDepthStencilView(
        DepthStencilView(), 
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 
        1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    // Draw imgui UI.
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, 
        &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    // Done recording commands.
    ThrowIfFailed(mCommandList->Close());

    // Add the command list to the queue for execution.
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Swap the back and front buffers
    DXGI_PRESENT_PARAMETERS presentParams = { 0 };
    ThrowIfFailed(mSwapChain->Present1(0, 0, &presentParams));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    // Wait until frame commands are complete.  This waiting is inefficient and is
    // done for simplicity.  Later we will show how to organize our rendering code
    // so we do not have to wait per frame.
    FlushCommandQueue();
}

void InitDirect3DApp::UpdateImgui(const GameTimer& gt)
{
    D3DApp::UpdateImgui(gt);

    //
    // Define a panel to render GUI elements.
    // 
    ImGui::Begin("Options");

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

    GraphicsMemoryStatistics gfxMemStats = GraphicsMemory::Get(
        md3dDevice.Get()).GetStatistics();
    
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

void InitDirect3DApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;

        SetCapture(mhMainWnd);
    }
}

void InitDirect3DApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        ReleaseCapture();
    }
}

void InitDirect3DApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureMouse)
    {
        mLastMousePos.x = x;
        mLastMousePos.y = y;
    }
}
 
void InitDirect3DApp::BuildCbvSrvUavDescriptorHeap()
{
    CbvSrvUavHeap& cbvSrvUavHeap = CbvSrvUavHeap::Get();
    cbvSrvUavHeap.Init(md3dDevice.Get(), CBV_SRV_UAV_HEAP_CAPACITY);

    InitImgui(cbvSrvUavHeap);
}