
#include "MaterialLib.h"
#include "d3dUtil.h"
#include <algorithm>

using namespace DirectX;
using namespace DirectX::SimpleMath;

bool MaterialLib::IsInitialized()const
{
    return mIsInitialized;
}

uint32_t MaterialLib::GetMaterialCount()const
{
    return static_cast<uint32_t>(mMaterials.size());
}

void MaterialLib::Init(ID3D12Device* device)
{
    TextureLib& texLib = TextureLib::GetLib();

    AddMaterial("whiteMat",
                texLib["defaultDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                XMFLOAT3(0.1f, 0.1f, 0.1f), 0.3f);

    AddMaterial("crate",
                texLib["crateDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                XMFLOAT3(0.1f, 0.1f, 0.1f), 0.3f);

    AddMaterial("water",
                texLib["waterDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 0.5f),
                XMFLOAT3(0.1f, 0.1f, 0.1f), 0.1f);

    AddMaterial("fence",
                texLib["fenceDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                XMFLOAT3(0.1f, 0.1f, 0.1f), 0.25f);

    AddMaterial("grass",
                texLib["grassDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                XMFLOAT3(0.1f, 0.1f, 0.1f), 0.8f);

    AddMaterial("bricks0", 
                texLib["bricksDiffuseMap"],
                texLib["bricksNormalMap"],
                texLib["bricksGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f), 
                XMFLOAT3(0.1f, 0.1f, 0.1f), 0.3f);

    AddMaterial("tile0", 
                texLib["tileDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f), 
                XMFLOAT3(0.2f, 0.2f, 0.2f), 
                0.1f,
                0.25f);

    AddMaterial("stoneFloor",
                texLib["stoneFloorDiffuseMap"],
                texLib["stoneFloorNormalMap"],
                texLib["stoneFloorGlossHeightAoMap"],
                XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f),
                XMFLOAT3(0.2f, 0.2f, 0.2f),
                0.1f,
                0.25f);

    AddMaterial("rock0",
                texLib["rock_color"],
                texLib["rock_normal"],
                texLib["rock_gloss_height_ao"],
                XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f),
                XMFLOAT3(0.2f, 0.2f, 0.2f), 0.1f);

    AddMaterial("mirror0", 
                texLib["defaultDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f), 
                XMFLOAT3(0.98f, 0.97f, 0.95f), 0.1f);

    AddMaterial("mirror1",
                texLib["defaultDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(0.1f, 0.1f, 0.3f, 1.0f),
                XMFLOAT3(0.4f, 0.4f, 0.4f), 0.05f);

    AddMaterial("skullMat", 
                texLib["defaultDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f), 
                XMFLOAT3(0.6f, 0.6f, 0.6f), 0.2f);

    AddMaterial("skullMatMatte",
                texLib["defaultDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(0.8f, 0.8f, 0.8f, 1.0f),
                XMFLOAT3(0.0f, 0.0f, 0.0f), 0.2f);

    AddMaterial("sky", 
                texLib["defaultDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f), 
                XMFLOAT3(0.1f, 0.1f, 0.1f), 1.0f);

    AddMaterial("highlight0",
                texLib["defaultDiffuseMap"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 0.0f, 0.5f),
                XMFLOAT3(0.1f, 0.1f, 0.1f), 0.0f);

    AddMaterial("treeSprites",
                texLib["treeSpritesArray"],
                texLib["defaultNormalMap"],
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                XMFLOAT3(0.01f, 0.01f, 0.01f), 0.125f);

    AddMaterial("columnRound",
                texLib["columnRoundDiffuseMap"],
                texLib["columnRoundNormalMap"],
                texLib["columnRoundGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                XMFLOAT3(0.01f, 0.01f, 0.01f), 0.125f);

    AddMaterial("columnSquare",
                texLib["columnSquareDiffuseMap"],
                texLib["columnSquareNormalMap"],
                texLib["columnSquareGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                XMFLOAT3(0.01f, 0.01f, 0.01f), 0.125f);

    AddMaterial("orbBase",
                texLib["orbBaseDiffuseMap"],
                texLib["orbBaseNormalMap"],
                texLib["orbBaseGlossHeightAoMap"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                XMFLOAT3(0.01f, 0.01f, 0.01f), 0.125f);


    //
    // Terrain layer materials.
    //

    AddMaterial("terrainlayer0", 
                texLib["grass_color"], 
                texLib["grass_normal"], 
                texLib["grass_gloss_height_ao"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f), XMFLOAT3(0.05f, 0.05f, 0.05f), 
                0.0f, 0.3f, 
                Matrix::CreateScale(128.0f));

    AddMaterial("terrainlayer1", 
                texLib["sand_color"], 
                texLib["sand_normal"], 
                texLib["sand_gloss_height_ao"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f), 
                XMFLOAT3(0.1f, 0.1f, 0.1f), 0.0f, 1.0f, 
                Matrix::CreateScale(128.0f));

    AddMaterial("terrainlayer2", 
                texLib["rock_color"], 
                texLib["rock_normal"], 
                texLib["rock_gloss_height_ao"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f), 
                XMFLOAT3(0.2f, 0.2f, 0.2f), 
                0.0f, 1.0f, 
                Matrix::CreateScale(32.0f));

    AddMaterial("terrainlayer3", 
                texLib["dirt0_color"], 
                texLib["dirt0_normal"], 
                texLib["dirt0_gloss_height_ao"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f), 
                XMFLOAT3(0.1f, 0.1f, 0.1f), 
                0.0f, 0.3f, 
                Matrix::CreateScale(64.0f));

    AddMaterial("terrainlayer4", 
                texLib["dirt1_color"], 
                texLib["dirt1_normal"], 
                texLib["dirt1_gloss_height_ao"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f), 
                XMFLOAT3(0.1f, 0.1f, 0.1f), 
                0.0f, 0.5f, 
                Matrix::CreateScale(128.0f));

    AddMaterial("terrainlayer5", 
                texLib["trail_color"], 
                texLib["trail_normal"], 
                texLib["trail_gloss_height_ao"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
                XMFLOAT3(0.1f, 0.1f, 0.1f), 
                0.0f, 0.2f, 
                Matrix::CreateScale(256.0f));

    AddMaterial("terrainlayer6", 
                texLib["rock1_color"], 
                texLib["rock1_normal"], 
                texLib["rock1_gloss_height_ao"],
                XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f), 
                XMFLOAT3(0.1f, 0.1f, 0.1f), 
                0.0f, 1.3f, 
                Matrix::CreateScale(32.0f));

    const float transparencyScale = 0.7f;
    const float indexOfRefraction = 1.06f;
    AddMaterial("glass0", 
                texLib["defaultDiffuseMap"], 
                texLib["defaultNormalMap"], 
                texLib["defaultGlossHeightAoMap"],
                XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f), 
                XMFLOAT3(0.4f, 0.4f, 0.4f), 
                0.1f, 
                1.0f, 
                MathHelper::Identity4x4(),
                transparencyScale, 
                indexOfRefraction);



    mIsInitialized = true;
}

bool MaterialLib::AddMaterial(const std::string& name, Texture* albedoMap, Texture* normalMap, Texture* glossHeightAoMap,
                              const XMFLOAT4& diffuse, const XMFLOAT3& fresnel, float roughness, 
                              float displacementScale, const DirectX::XMFLOAT4X4& matTransform,
                              float transparency, float indexOfRefraction)
{
    if(mMaterials.find(name) == mMaterials.end())
    {
        static int matIndex = 0;

        auto mat = std::make_unique<Material>();
        mat->Name = name;
        mat->MatIndex = matIndex;
        mat->AlbedoBindlessIndex = albedoMap != nullptr ? albedoMap->BindlessIndex : -1;
        mat->NormalBindlessIndex = normalMap != nullptr ? normalMap->BindlessIndex : -1;
        mat->GlossHeightAoBindlessIndex = glossHeightAoMap != nullptr ? glossHeightAoMap->BindlessIndex : -1;

        mat->DiffuseAlbedo = diffuse;
        mat->FresnelR0 = fresnel;
        mat->Roughness = roughness;
        mat->DisplacementScale = displacementScale;
        mat->MatTransform = matTransform;

        // Used in ray tracing demos only.
        mat->TransparencyWeight = transparency;
        mat->IndexOfRefraction = indexOfRefraction;

        matIndex++;

        mMaterials[name] = std::move(mat);
        return true;
    }

    return false;
}

Material* MaterialLib::operator[](const std::string& name)
{
    if(mMaterials.find(name) != mMaterials.end())
    {
        return mMaterials[name].get();
    }

    return nullptr;
}

const std::unordered_map<std::string, std::unique_ptr<Material>>& MaterialLib::GetCollection()const
{
    return mMaterials;
}

