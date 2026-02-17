#pragma once

#include <windows.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <d3d12.h>
#include "d3dx12.h"
#include "dxc/inc/dxcapi.h"
#include "dxc/inc/d3d12shader.h"
#include <unordered_map>

// Creates all shaders used in the book demos in one place so we do not 
// have to duplicate across demos.
class ShaderLib
{
public:
    ShaderLib(const ShaderLib& rhs) = delete;
    ShaderLib& operator=(const ShaderLib& rhs) = delete;

    static ShaderLib& GetLib()
    {
        static ShaderLib singleton;
        return singleton;
    }

    bool IsInitialized()const;

	void Init(ID3D12Device* device);

    bool AddShader(const std::string& name, Microsoft::WRL::ComPtr<IDxcBlob> shader);

    IDxcBlob* operator[](const std::string& name);

private:
    ShaderLib() = default;

protected:
    bool mIsInitialized = false;

    std::unordered_map<std::string, Microsoft::WRL::ComPtr<IDxcBlob>> mShaders;
};


