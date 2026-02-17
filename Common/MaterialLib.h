#pragma once

#include <windows.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <d3d12.h>
#include "d3dx12.h"
#include "dxc/inc/dxcapi.h"
#include "dxc/inc/d3d12shader.h"
#include "d3dUtil.h"
#include "TextureLib.h"
#include <unordered_map>
#include <memory>

// Creates all materials used in the book demos in one place so we do not 
// have to duplicate across demos.
class MaterialLib
{
public:
    MaterialLib(const MaterialLib& rhs) = delete;
    MaterialLib& operator=(const MaterialLib& rhs) = delete;

    static MaterialLib& GetLib()
    {
        static MaterialLib singleton;
        return singleton;
    }

    bool IsInitialized()const;
    uint32_t GetMaterialCount()const;

	void Init(ID3D12Device* device);

    bool AddMaterial(const std::string& name, Texture* albedoMap, Texture* normalMap, Texture* glossHeightAoMap,
                     const DirectX::XMFLOAT4& diffuse, const DirectX::XMFLOAT3& fresnel, float roughness,
                     float displacementScale = 1.0f, 
                     const DirectX::XMFLOAT4X4& matTransform = MathHelper::Identity4x4(), 
                     float transparency = 0.0f, 
                     float indexOfRefraction = 0.0f);

    Material* operator[](const std::string& name);

    const std::unordered_map<std::string, std::unique_ptr<Material>>& GetCollection()const;
private:
    MaterialLib() = default;

protected:
    bool mIsInitialized = false;

    std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
};


