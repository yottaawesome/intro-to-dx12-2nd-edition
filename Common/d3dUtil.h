//***************************************************************************************
// d3dUtil.h by Frank Luna (C) 2015 All Rights Reserved.
//
// General helper code.
//***************************************************************************************

#pragma once

#define NOMINMAX

#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <DirectXColors.h>
#include <DirectXCollision.h>
#include <string>
#include <memory>
#include <algorithm>
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cassert>
#include "d3dx12.h"
#include "dxc/inc/dxcapi.h"
#include "dxc/inc/d3d12shader.h"
#include "MathHelper.h"
#include "MeshUtil.h"
#include "Random.h"

#include "DirectXTK12/Inc/SimpleMath.h"
#include "DirectXTK12/Inc/BufferHelpers.h"
#include "DirectXTK12/Inc/ResourceUploadBatch.h"
#include "DirectXTK12/Inc/DDSTextureLoader.h"
#include "DirectXTK12/Inc/DirectXHelpers.h"
#include "DirectXTK12/Inc/GraphicsMemory.h"
#include "DirectXTK12/Inc/SpriteBatch.h"

extern const int gNumFrameResources;

inline constexpr DXGI_FORMAT SsaoAmbientMapFormat = DXGI_FORMAT_R16_UNORM;
inline constexpr DXGI_FORMAT SceneNormalMapFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

inline void d3dSetDebugName(IDXGIObject* obj, const char* name)
{
    if(obj)
    {
        obj->SetPrivateData(WKPDID_D3DDebugObjectName, lstrlenA(name), name);
    }
}
inline void d3dSetDebugName(ID3D12Device* obj, const char* name)
{
    if(obj)
    {
        obj->SetPrivateData(WKPDID_D3DDebugObjectName, lstrlenA(name), name);
    }
}
inline void d3dSetDebugName(ID3D12DeviceChild* obj, const char* name)
{
    if(obj)
    {
        obj->SetPrivateData(WKPDID_D3DDebugObjectName, lstrlenA(name), name);
    }
}

inline std::wstring AnsiToWString(const std::string& str)
{
    WCHAR buffer[512];
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, buffer, 512);
    return std::wstring(buffer);
}

class d3dUtil
{
public:

    static void WriteBinaryToFile(IDxcBlob* blob, const std::wstring& filename);

    static bool IsKeyDown(int vkeyCode);

    static std::string ToString(HRESULT hr);

    static UINT Align(UINT size, UINT alignment)
    {
        return (size + (alignment - 1)) & ~(alignment - 1);
    }

    static UINT CalcConstantBufferByteSize(UINT byteSize)
    {
        // Constant buffers must be a multiple of the minimum hardware
        // allocation size (usually 256 bytes).  So round up to nearest
        // multiple of 256.  We do this by adding 255 and then masking off
        // the lower 2 bytes which store all bits < 256.
        // Example: Suppose byteSize = 300.
        // (300 + 255) & ~255
        // 555 & ~255
        // 0x022B & ~0x00ff
        // 0x022B & 0xff00
        // 0x0200
        // 512
        return Align(byteSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    }

    // Uses dxc for shader model 6.0+. compileArgs is same as you would pass to dxc command line.
    // For example: "-E main -T ps_6_0 -Zi -Fd pdbPath -D mydefine=1"
    // There are also helper strings defined in dxcapi.h (partial list):
    // #define DXC_ARG_DEBUG L"-Zi"
    // #define DXC_ARG_SKIP_VALIDATION L"-Vd"
    // #define DXC_ARG_SKIP_OPTIMIZATIONS L"-Od"
    // #define DXC_ARG_PACK_MATRIX_ROW_MAJOR L"-Zpr"
    // #define DXC_ARG_PACK_MATRIX_COLUMN_MAJOR L"-Zpc"
    static Microsoft::WRL::ComPtr<IDxcBlob> CompileShader(const std::wstring& filename, std::vector<LPCWSTR>& compileArgs);

    static D3D12_SHADER_BYTECODE ByteCodeFromBlob(IDxcBlob* shader)
    {
        return { reinterpret_cast<BYTE*>(shader->GetBufferPointer()), shader->GetBufferSize() };
    }

    // Helper function for most of the common case code to fill out PSO description.
    // Modify the return value as needed to customize.
    static D3D12_GRAPHICS_PIPELINE_STATE_DESC InitDefaultPso(DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat,
                                                             const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout, ID3D12RootSignature* rootSig,
                                                             IDxcBlob* vertexShader, IDxcBlob* pixelShader);

    static Microsoft::WRL::ComPtr<ID3D12Resource> CreateRandomTexture(
        ID3D12Device* device, DirectX::ResourceUploadBatch& resourceUpload,
        size_t width, size_t height);

    static std::unique_ptr<MeshGeometry> BuildShapeGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, bool useIndex32 = false);
    static std::unique_ptr<MeshGeometry> BuildSkullGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch);
    static std::unique_ptr<MeshGeometry> LoadSimpleModelGeometry(
        ID3D12Device* device, 
        DirectX::ResourceUploadBatch& uploadBatch, 
        const std::string& filename, 
        const std::string& geoName,
        bool useIndex32 = false);
    static std::vector<float> CalcGaussWeights(float sigma);
};

struct ModelVertex
{
    ModelVertex() = default;
    ModelVertex(
        float px, float py, float pz,
        float nx, float ny, float nz,
        float u, float v) :
        Pos(px, py, pz),
        Normal(nx, ny, nz),
        TexC(u, v)
    {
    }

    DirectX::XMFLOAT3 Pos {};
    DirectX::XMFLOAT3 Normal {};
    DirectX::XMFLOAT2 TexC {};
    DirectX::XMFLOAT3 TangentU {};
};

// Simple struct to represent a material for our demos. 
struct Material
{
    // Unique material name for lookup.
    std::string Name;

    // Index into material buffer.
    int MatIndex = -1;

    // For bindless texturing.
    int AlbedoBindlessIndex = -1;
    int NormalBindlessIndex = -1;
    int GlossHeightAoBindlessIndex = -1;

    // Dirty flag indicating the material has changed and we need to update the buffer.
    // Because we have a material buffer for each FrameResource, we have to apply the
    // update to each FrameResource.  Thus, when we modify a material we should set 
    // NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
    int NumFramesDirty = gNumFrameResources;

    // Material constant buffer data used for shading.
    DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 FresnelR0 = { 0.01f, 0.01f, 0.01f };
    float Roughness = .25f;
    float DisplacementScale = 1.0f;
    DirectX::XMFLOAT4X4 MatTransform = MathHelper::Identity4x4();

    // Used in ray tracing demos only.
    float TransparencyWeight = 0.0f;
    float IndexOfRefraction = 0.0f;
};

class DxException
{
public:
    DxException() = default;
    DxException(HRESULT hr, const std::wstring& functionName, const std::wstring& filename, int lineNumber) :
        ErrorCode(hr),
        FunctionName(functionName),
        Filename(filename),
        LineNumber(lineNumber)
    {
    }

    std::wstring ToString()const;

    HRESULT ErrorCode = S_OK;
    std::wstring FunctionName;
    std::wstring Filename;
    int LineNumber = -1;
};

#ifndef ThrowIfFailed
#define ThrowIfFailed(x)                                              \
{                                                                     \
    HRESULT hr__ = (x);                                               \
    std::wstring wfn = AnsiToWString(__FILE__);                       \
    if(FAILED(hr__)) { throw DxException(hr__, L#x, wfn, __LINE__); } \
}
#endif

#ifndef ReleaseCom
#define ReleaseCom(x) { if(x){ x->Release(); x = 0; } }
#endif