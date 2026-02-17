
#include "d3dUtil.h"
#include "MeshGen.h"
#include "LoadM3d.h"
#include <comdef.h>
#include <fstream>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

void d3dUtil::WriteBinaryToFile(IDxcBlob* blob, const std::wstring& filename)
{
    std::ofstream fout(filename, std::ios::binary);
    fout.write((char*)blob->GetBufferPointer(), blob->GetBufferSize());
    fout.close();
}

bool d3dUtil::IsKeyDown(int vkeyCode)
{
    return (GetAsyncKeyState(vkeyCode) & 0x8000) != 0;
}

// See "HLSL Compiler | Michael Dougherty | DirectX Developer Day"
// https://www.youtube.com/watch?v=tyyKeTsdtmo
ComPtr<IDxcBlob> d3dUtil::CompileShader(
    const std::wstring& filename,
    std::vector<LPCWSTR>& compileArgs)
{
    static ComPtr<IDxcUtils> utils = nullptr;
    static ComPtr<IDxcCompiler3> compiler = nullptr;
    static ComPtr<IDxcIncludeHandler> defaultIncludeHandler = nullptr;

    if(!std::filesystem::exists(filename))
    {
        std::wstring msg = filename + L" not found.";
        OutputDebugStringW(msg.c_str());
        MessageBox(0, msg.c_str(), 0, 0);
    }

    // Only need one of these.
    if (compiler == nullptr)
    {
        ThrowIfFailed(DxcCreateInstance(
            CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
        ThrowIfFailed(DxcCreateInstance(
            CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
        ThrowIfFailed(utils->CreateDefaultIncludeHandler(
            &defaultIncludeHandler));
    }

    // Use IDxcUtils to load the text file.
    uint32_t codePage = CP_UTF8;
    ComPtr<IDxcBlobEncoding> sourceBlob = nullptr;
    ThrowIfFailed(utils->LoadFile(filename.c_str(), &codePage, &sourceBlob));

    // Create a DxcBuffer buffer to the source code.
    DxcBuffer sourceBuffer;
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = 0;

    ComPtr<IDxcResult> result = nullptr;
    HRESULT hr = compiler->Compile(
        &sourceBuffer,               // source code
        compileArgs.data(),          // arguments
        (UINT)compileArgs.size(),    // argument count
        defaultIncludeHandler.Get(), // include handler
        IID_PPV_ARGS(result.GetAddressOf())); // output

    if(SUCCEEDED(hr))
        result->GetStatus(&hr);

    // Get errors and output them if any.
    ComPtr<IDxcBlobUtf8> errorMsgs = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, 
                      IID_PPV_ARGS(&errorMsgs), nullptr);

    if (errorMsgs && errorMsgs->GetStringLength())
    {
        std::wstring errorText = AnsiToWString(errorMsgs->GetStringPointer());

        // replace the hlsl.hlsl placeholder in the error string with the shader filename.
        std::wstring dummyFilename = L"hlsl.hlsl";
        errorText.replace(errorText.find(dummyFilename), dummyFilename.length(), filename);

        OutputDebugStringW(errorText.c_str());
        ThrowIfFailed(E_FAIL);
    }

    // Get the DX intermediate language, which the GPU driver will translate
    // into native GPU code.
    ComPtr<IDxcBlob> dxil = nullptr;
    ThrowIfFailed(result->GetOutput(DXC_OUT_OBJECT,
                  IID_PPV_ARGS(&dxil), nullptr));

#if defined(DEBUG) || defined(_DEBUG)  
    // Write PDB data for PIX debugging.
    const std::string pdbDirectory = "HLSL PDB/";
    if(!std::filesystem::exists(pdbDirectory))
    {
        std::filesystem::create_directory(pdbDirectory);
    }

    ComPtr<IDxcBlob> pdbData = nullptr;
    ComPtr<IDxcBlobUtf16> pdbPathFromCompiler = nullptr;
    ThrowIfFailed(result->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pdbData), &pdbPathFromCompiler));
    WriteBinaryToFile(pdbData.Get(), 
        AnsiToWString(pdbDirectory) + 
        std::wstring(pdbPathFromCompiler->GetStringPointer()));
#endif

    // Return the data blob containing the DXIL code.
    return dxil;
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC d3dUtil::InitDefaultPso(DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, 
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout, ID3D12RootSignature* rootSig, 
    IDxcBlob* vertexShader, IDxcBlob* pixelShader)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
    ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

    psoDesc.InputLayout = { inputLayout.data(), (UINT)inputLayout.size() };
    psoDesc.pRootSignature = rootSig;
    psoDesc.VS = ByteCodeFromBlob(vertexShader);
    psoDesc.PS = ByteCodeFromBlob(pixelShader);

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtvFormat;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    psoDesc.DSVFormat = dsvFormat;

    return psoDesc;
}

ComPtr<ID3D12Resource> d3dUtil::CreateRandomTexture(
    ID3D12Device* device, ResourceUploadBatch& resourceUpload,
    size_t width, size_t height)
{
    std::vector<XMCOLOR> initData(width * height);
    for(int i = 0; i < height; ++i)
    {
        for(int j = 0; j < width; ++j)
        {
            // Random vector in [0,1).
            DirectX::XMFLOAT4 v(
                MathHelper::RandF(), 
                MathHelper::RandF(), 
                MathHelper::RandF(), 
                MathHelper::RandF());

            initData[i * width + j] = XMCOLOR(v.x, v.y, v.z, v.w);
        }
    }

    D3D12_SUBRESOURCE_DATA subResourceData = {};
    subResourceData.pData = initData.data();
    subResourceData.RowPitch = width * sizeof(XMCOLOR);
    subResourceData.SlicePitch = subResourceData.RowPitch * width;

    ComPtr<ID3D12Resource> randomTex;
    ThrowIfFailed(CreateTextureFromMemory(device,
                  resourceUpload,
                  width, height,
                  DXGI_FORMAT_R8G8B8A8_UNORM,
                  subResourceData,
                  &randomTex));

    return randomTex;
}

std::unique_ptr<MeshGeometry> d3dUtil::BuildShapeGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch, bool useIndex32)
{
    MeshGen meshGen;
    MeshGenData box = meshGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
    MeshGenData grid = meshGen.CreateGrid(20.0f, 30.0f, 30, 20);
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

    // Extract the vertex elements we are interested into our vertex buffer. 
    std::vector<ModelVertex> vertices(compositeMesh.Vertices.size());
    for(size_t i = 0; i < compositeMesh.Vertices.size(); ++i)
    {
        vertices[i].Pos = compositeMesh.Vertices[i].Position;
        vertices[i].Normal = compositeMesh.Vertices[i].Normal;
        vertices[i].TexC = compositeMesh.Vertices[i].TexC;
        vertices[i].TangentU = compositeMesh.Vertices[i].TangentU;
    }

    const uint32_t indexCount = (UINT)compositeMesh.Indices32.size();

    const UINT indexElementByteSize = useIndex32 ? sizeof(uint32_t) : sizeof(uint16_t);
    const UINT vbByteSize = (UINT)vertices.size() * sizeof(ModelVertex);
    const UINT ibByteSize = indexCount * indexElementByteSize;

    const byte* indexData = useIndex32 ?
        reinterpret_cast<byte*>(compositeMesh.Indices32.data()) :
        reinterpret_cast<byte*>(compositeMesh.GetIndices16().data());

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "shapeGeo";

    geo->VertexBufferCPU.resize(vbByteSize);
    CopyMemory(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

    geo->IndexBufferCPU.resize(ibByteSize);
    CopyMemory(geo->IndexBufferCPU.data(), indexData, ibByteSize);

    CreateStaticBuffer(device, uploadBatch,
                       vertices.data(), vertices.size(), sizeof(ModelVertex),
                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &geo->VertexBufferGPU);

    CreateStaticBuffer(device, uploadBatch,
                       indexData, indexCount, indexElementByteSize,
                       D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);

    geo->VertexByteStride = sizeof(ModelVertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = useIndex32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    geo->DrawArgs["box"] = boxSubmesh;
    geo->DrawArgs["grid"] = gridSubmesh;
    geo->DrawArgs["sphere"] = sphereSubmesh;
    geo->DrawArgs["cylinder"] = cylinderSubmesh;
    geo->DrawArgs["quad"] = quadSubmesh;

    return geo;
}

std::unique_ptr<MeshGeometry> d3dUtil::BuildSkullGeometry(ID3D12Device* device, DirectX::ResourceUploadBatch& uploadBatch)
{
    std::ifstream fin("Models/skull.txt");

    if(!fin)
    {
        MessageBox(0, L"Models/skull.txt not found.", 0, 0);
        return nullptr;
    }

    UINT vcount = 0;
    UINT tcount = 0;
    std::string ignore;

    fin >> ignore >> vcount;
    fin >> ignore >> tcount;
    fin >> ignore >> ignore >> ignore >> ignore;

    XMFLOAT3 vMinf3(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
    XMFLOAT3 vMaxf3(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);

    XMVECTOR vMin = XMLoadFloat3(&vMinf3);
    XMVECTOR vMax = XMLoadFloat3(&vMaxf3);

    std::vector<ModelVertex> vertices(vcount);
    for(UINT i = 0; i < vcount; ++i)
    {
        fin >> vertices[i].Pos.x >> vertices[i].Pos.y >> vertices[i].Pos.z;
        fin >> vertices[i].Normal.x >> vertices[i].Normal.y >> vertices[i].Normal.z;

        XMVECTOR P = XMLoadFloat3(&vertices[i].Pos);

        // Project point onto unit sphere and generate spherical texture coordinates.
        XMFLOAT3 spherePos;
        XMStoreFloat3(&spherePos, XMVector3Normalize(P));

        XMVECTOR N = XMLoadFloat3(&vertices[i].Normal);

        // Generate a tangent vector so normal mapping works.  We aren't applying
        // a texture map to the skull, so we just need any tangent vector so that
        // the math works out to give us the original interpolated vertex normal.
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if(fabsf(XMVectorGetX(XMVector3Dot(N, up))) < 1.0f - 0.001f)
        {
            XMVECTOR T = XMVector3Normalize(XMVector3Cross(up, N));
            XMStoreFloat3(&vertices[i].TangentU, T);
        }
        else
        {
            up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            XMVECTOR T = XMVector3Normalize(XMVector3Cross(N, up));
            XMStoreFloat3(&vertices[i].TangentU, T);
        }

        // The skull mesh does not have defined texture coordinates, but 
        // we can auto generate some. We generate sphereical projection
        // texture coordinates by projecting the vertices onto the unit
        // sphere. Because the skull is not a sphere, there will be some
        // distortion from this transformation, but it gives reasonable 
        // texture coordinates when we have none.

        float theta = atan2f(spherePos.z, spherePos.x);

        // Put in [0, 2pi].
        if(theta < 0.0f)
            theta += XM_2PI;

        float phi = acosf(spherePos.y);

        float u = theta / (2.0f*XM_PI);
        float v = phi / XM_PI;

        vertices[i].TexC = { u, v };

        vMin = XMVectorMin(vMin, P);
        vMax = XMVectorMax(vMax, P);
    }

    BoundingBox bounds;
    XMStoreFloat3(&bounds.Center, 0.5f*(vMin + vMax));
    XMStoreFloat3(&bounds.Extents, 0.5f*(vMax - vMin));

    fin >> ignore;
    fin >> ignore;
    fin >> ignore;

    std::vector<std::int32_t> indices(3 * tcount);
    for(UINT i = 0; i < tcount; ++i)
    {
        fin >> indices[i * 3 + 0] >> indices[i * 3 + 1] >> indices[i * 3 + 2];
    }

    fin.close();


    const UINT vbByteSize = (UINT)vertices.size() * sizeof(ModelVertex);

    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::int32_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "skullGeo";

    geo->VertexBufferCPU.resize(vbByteSize);
    CopyMemory(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

    geo->IndexBufferCPU.resize(ibByteSize);
    CopyMemory(geo->IndexBufferCPU.data(), indices.data(), ibByteSize);

    CreateStaticBuffer(device, uploadBatch,
                       vertices.data(), vertices.size(), sizeof(ModelVertex),
                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &geo->VertexBufferGPU);

    CreateStaticBuffer(device, uploadBatch,
                       indices.data(), indices.size(), sizeof(std::uint32_t),
                       D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);

    geo->VertexByteStride = sizeof(ModelVertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    submesh.VertexCount = (UINT)vertices.size();
    submesh.Bounds = bounds;

    geo->DrawArgs["skull"] = submesh;

    return geo;
}

std::unique_ptr<MeshGeometry> d3dUtil::LoadSimpleModelGeometry(
    ID3D12Device* device, 
    DirectX::ResourceUploadBatch& uploadBatch,
    const std::string& filename, 
    const std::string& geoName, 
    bool useIndex32)
{
    std::vector<M3DLoader::Vertex> m3dVertices;
    std::vector<UINT> indices32;
    std::vector<M3DLoader::Subset> subsets;
    std::vector<M3DLoader::M3dMaterial> mats;

    M3DLoader loader;
    loader.LoadM3d(filename, m3dVertices, indices32, subsets, mats);

    // Assume simple model has one subset and one material.
    assert(subsets.size() == 1);
    assert(mats.size() == 1);

    XMFLOAT3 vMinf3(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
    XMFLOAT3 vMaxf3(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);

    XMVECTOR vMin = XMLoadFloat3(&vMinf3);
    XMVECTOR vMax = XMLoadFloat3(&vMaxf3);

    std::vector<ModelVertex> vertices(m3dVertices.size());
    for(UINT i = 0; i < m3dVertices.size(); ++i)
    {
        XMVECTOR P = XMLoadFloat3(&m3dVertices[i].Pos);

        vertices[i].Pos = m3dVertices[i].Pos;
        vertices[i].Normal = m3dVertices[i].Normal;
        vertices[i].TangentU = XMFLOAT3(m3dVertices[i].TangentU.x, m3dVertices[i].TangentU.y, m3dVertices[i].TangentU.z);
        vertices[i].TexC = m3dVertices[i].TexC;

        vMin = XMVectorMin(vMin, P);
        vMax = XMVectorMax(vMax, P);
    }

    BoundingBox bounds;
    XMStoreFloat3(&bounds.Center, 0.5f*(vMin + vMax));
    XMStoreFloat3(&bounds.Extents, 0.5f*(vMax - vMin));

    const UINT indexElementByteSize = useIndex32 ? sizeof(uint32_t) : sizeof(uint16_t);
    const UINT vbByteSize = (UINT)vertices.size() * sizeof(ModelVertex);
    const UINT ibByteSize = (UINT)indices32.size() * indexElementByteSize;

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = geoName;

    geo->VertexBufferCPU.resize(vbByteSize);
    CopyMemory(geo->VertexBufferCPU.data(), vertices.data(), vbByteSize);

    CreateStaticBuffer(device, uploadBatch,
                       vertices.data(), vertices.size(), sizeof(ModelVertex),
                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &geo->VertexBufferGPU);

    if(useIndex32)
    {
        geo->IndexBufferCPU.resize(ibByteSize);
        CopyMemory(geo->IndexBufferCPU.data(), indices32.data(), ibByteSize);

        CreateStaticBuffer(
            device, uploadBatch,
            indices32.data(), indices32.size(), sizeof(uint32_t),
            D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);
    }
    else
    {
        std::vector<USHORT> indices16(indices32.size());
        std::transform(std::begin(indices32), std::end(indices32), std::begin(indices16), [](UINT x)
        {
            return static_cast<USHORT>(x);
        });

        geo->IndexBufferCPU.resize(ibByteSize);
        CopyMemory(geo->IndexBufferCPU.data(), indices16.data(), ibByteSize);

        CreateStaticBuffer(
            device, uploadBatch,
            indices16.data(), indices16.size(), sizeof(uint16_t),
            D3D12_RESOURCE_STATE_INDEX_BUFFER, &geo->IndexBufferGPU);
    }

    geo->VertexByteStride = sizeof(ModelVertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = useIndex32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices32.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    submesh.VertexCount = (UINT)vertices.size();
    submesh.Bounds = bounds;

    geo->DrawArgs["subset0"] = submesh;

    return geo;
}


std::vector<float> d3dUtil::CalcGaussWeights(float sigma)
{
    float twoSigma2 = 2.0f*sigma*sigma;

    // Estimate the blur radius based on sigma since sigma controls the "width" of the bell curve.
    // For example, for sigma = 3, the width of the bell curve is 
    int blurRadius = (int)ceil(2.0f * sigma);

    std::vector<float> weights;
    weights.resize(2 * blurRadius + 1);

    float weightSum = 0.0f;

    for(int i = -blurRadius; i <= blurRadius; ++i)
    {
        float x = (float)i;

        weights[i + blurRadius] = expf(-x*x / twoSigma2);

        weightSum += weights[i + blurRadius];
    }

    // Divide by the sum so all the weights add up to 1.0.
    for(int i = 0; i < weights.size(); ++i)
    {
        weights[i] /= weightSum;
    }

    return weights;
}

std::wstring DxException::ToString()const
{
    // Get the string description of the error code.
    _com_error err(ErrorCode);
    std::wstring msg = err.ErrorMessage();

    return FunctionName + L" failed in " + Filename + L"; line " + std::to_wstring(LineNumber) + L"; error: " + msg;
}

