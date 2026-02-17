#pragma once

#include <windows.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <d3d12.h>
#include "d3dx12.h"
#include <unordered_map>
#include <memory>
#include "d3dUtil.h"

struct Texture
{
    Texture() = default;
    Texture(const Texture& rhs) = delete;
    Texture& operator = (const Texture& rhs) = delete;

    // Unique material name for lookup.
    std::string Name;

    std::wstring Filename;

    bool IsCubeMap = false;

    int BindlessIndex = -1;

    const D3D12_RESOURCE_DESC& Info()const { return Resource->GetDesc(); }

    Microsoft::WRL::ComPtr<ID3D12Resource> Resource = nullptr;
};

// Creates all textures used in the book demos in one place so we do not 
// have to duplicate across demos.
class TextureLib
{
public:
    TextureLib(const TextureLib& rhs) = delete;
    TextureLib& operator=(const TextureLib& rhs) = delete;

    static TextureLib& GetLib()
    {
        static TextureLib singleton;
        return singleton;
    }

    bool IsInitialized()const;

	void Init(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch);

    bool Contains(const std::string& name);

    bool AddTexture(const std::string& name, std::unique_ptr<Texture> tex);

    Texture* operator[](const std::string& name);

    const std::unordered_map<std::string, std::unique_ptr<Texture>>& GetCollection()const;
private:
    TextureLib() = default;

protected:
    bool mIsInitialized = false;

    std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
};


