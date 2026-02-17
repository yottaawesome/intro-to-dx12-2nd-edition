#pragma once

#include <windows.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <d3d12.h>
#include "d3dx12.h"
#include "dxc/inc/dxcapi.h"
#include "dxc/inc/d3d12shader.h"
#include <unordered_map>

// Creates all PSOs used in the book demos in one place so we do not 
// have to duplicate across demos.
class PsoLib
{
public:
    PsoLib(const PsoLib& rhs) = delete;
    PsoLib& operator=(const PsoLib& rhs) = delete;

    static PsoLib& GetLib()
    {
        static PsoLib singleton;
        return singleton;
    }

    bool IsInitialized()const;

	void Init(ID3D12Device5* device,
              DXGI_FORMAT backBufferFormat, 
              DXGI_FORMAT depthStencilFormat, 
              DXGI_FORMAT ambientMapFormat,
              DXGI_FORMAT screenNormalMapFormat,
              ID3D12RootSignature* rootSig,
              ID3D12RootSignature* computeRootSig = nullptr);

    bool AddPso(const std::string& name, Microsoft::WRL::ComPtr<ID3D12PipelineState> pso);

    ID3D12PipelineState* operator[](const std::string& name);

private:
    PsoLib() = default;

    void InitHelixParticleMeshShaderPSOs(
        ID3D12Device5* device,
        DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat,
        ID3D12RootSignature* rootSig);

    void InitTerrainMeshShaderPSOs(
        ID3D12Device5* device,
        DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat,
        ID3D12RootSignature* rootSig);

protected:
    bool mIsInitialized = false;

    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D12PipelineState>> mPSOs;
};


