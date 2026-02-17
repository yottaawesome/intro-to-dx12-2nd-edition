
#include "TextureLib.h"
#include "d3dUtil.h"
#include <algorithm>

using namespace DirectX;

bool TextureLib::IsInitialized()const
{
    return mIsInitialized;
}

void TextureLib::Init(ID3D12Device* device, ResourceUploadBatch& uploadBatch)
{
    std::vector<std::string> texNames =
    {
        "crateDiffuseMap",
        "waterDiffuseMap",
        "fenceDiffuseMap",
        "grassDiffuseMap",

        "bricksDiffuseMap",
        "bricksNormalMap",
        "bricksGlossHeightAoMap",

        "tileDiffuseMap",

        "stoneFloorDiffuseMap",
        "stoneFloorNormalMap",
        "stoneFloorGlossHeightAoMap",

        "checkboardMap",
        "iceMap",

        "treeSpritesArray",

        "defaultDiffuseMap",
        "defaultNormalMap",
        "defaultGlossHeightAoMap",

        "rainParticle",
        "explosionParticle",
        "boltParticles",
        "skyCubeMap",

        "blendMap0",
        "blendMap1",

        "grass_color",
        "grass_normal",
        "grass_gloss_height_ao",

        "sand_color",
        "sand_normal",
        "sand_gloss_height_ao",

        "rock_color",
        "rock_normal",
        "rock_gloss_height_ao",

        "dirt0_color",
        "dirt0_normal",
        "dirt0_gloss_height_ao",

        "dirt1_color",
        "dirt1_normal",
        "dirt1_gloss_height_ao",

        "trail_color",
        "trail_normal",
        "trail_gloss_height_ao",

        "rock1_color",
        "rock1_normal",
        "rock1_gloss_height_ao",

        "columnRoundDiffuseMap",
        "columnRoundNormalMap",
        "columnRoundGlossHeightAoMap",

        "columnSquareDiffuseMap",
        "columnSquareNormalMap",
        "columnSquareGlossHeightAoMap",

        "orbBaseDiffuseMap",
        "orbBaseNormalMap",
        "orbBaseGlossHeightAoMap",
    };

    std::vector<std::wstring> texFilenames =
    {
        L"Textures/WoodCrate01.dds",
        L"Textures/water1.dds",
        L"Textures/WireFence.dds",
        L"Textures/grass.dds",

        L"Textures/bricks0_color.dds",
        L"Textures/bricks0_normal.dds",
        L"Textures/bricks0_gloss_height_ao.dds",

        L"Textures/tile0.dds",

        L"Textures/stonefloor1_diffuse.dds",
        L"Textures/stonefloor1_normal.dds",
        L"Textures/stonefloor1_gloss_height_ao.dds",

        L"Textures/checkboard.dds",
        L"Textures/ice.dds",

        L"Textures/treeArray2.dds",

        L"Textures/white1x1.dds",
        L"Textures/default_nmap.dds",
        L"Textures/default_glossHeightAoMap.dds",

        L"Textures/raindrop.dds",
        L"Textures/explosion.dds",
        L"Textures/bolt.dds",

        L"Textures/cubemaps/cubemap_sunset2.dds",

        L"Textures/terrain/blendmap0.dds",
        L"Textures/terrain/blendmap1.dds",

        L"Textures/terrain/grass0_color.dds",
        L"Textures/terrain/grass0_normal.dds",
        L"Textures/terrain/grass0_gloss_height_ao.dds",

        L"Textures/terrain/sand0_color.dds",
        L"Textures/terrain/sand0_normal.dds",
        L"Textures/terrain/sand0_gloss_height_ao.dds",

        L"Textures/terrain/rock0_color.dds",
        L"Textures/terrain/rock0_normal.dds",
        L"Textures/terrain/rock0_gloss_height_ao.dds",

        L"Textures/terrain/dirt0_color.dds",
        L"Textures/terrain/dirt0_normal.dds",
        L"Textures/terrain/dirt0_gloss_height_ao.dds",

        L"Textures/terrain/dirt1_color.dds",
        L"Textures/terrain/dirt1_normal.dds",
        L"Textures/terrain/dirt1_gloss_height_ao.dds",

        L"Textures/terrain/gravel0_color.dds",
        L"Textures/terrain/gravel0_normal.dds",
        L"Textures/terrain/gravel0_gloss_height_ao.dds",

        L"Textures/terrain/rock1_color.dds",
        L"Textures/terrain/rock1_normal.dds",
        L"Textures/terrain/rock1_gloss_height_ao.dds",

        L"Textures/models/columnRound_diffuse.dds",
        L"Textures/models/columnRound_normal.dds",
        L"Textures/models/columnRound_gloss_height_ao.dds",

        L"Textures/models/columnSquare_diffuse.dds",
        L"Textures/models/columnSquare_normal.dds",
        L"Textures/models/columnSquare_gloss_height_ao.dds",

        L"Textures/models/orbBase_diffuse.dds",
        L"Textures/models/orbBase_normal.dds",
        L"Textures/models/orbBase_gloss_height_ao.dds",
    };

    for(int i = 0; i < (int)texNames.size(); ++i)
    {
        auto texMap = std::make_unique<Texture>();
        texMap->Name = texNames[i];
        texMap->Filename = texFilenames[i];

        if(!std::filesystem::exists(texFilenames[i]))
        {
            std::wstring msg = texFilenames[i] + L" not found.";
            OutputDebugStringW(msg.c_str());
            MessageBox(0, msg.c_str(), 0, 0);
        }

        ThrowIfFailed(DirectX::CreateDDSTextureFromFileEx(
            device, uploadBatch,
            texMap->Filename.c_str(), 0, D3D12_RESOURCE_FLAG_NONE, 
            DDS_LOADER_DEFAULT,
            &texMap->Resource, nullptr, &texMap->IsCubeMap));

        mTextures[texMap->Name] = std::move(texMap);
    }

    auto randomTex = std::make_unique<Texture>();
    randomTex->Name = "randomTex1024";
    randomTex->Filename = L"";
    randomTex->IsCubeMap = false;
    randomTex->Resource = d3dUtil::CreateRandomTexture(device, uploadBatch, 1024, 1024);

    mTextures[randomTex->Name] = std::move(randomTex);

    mIsInitialized = true;
}

bool TextureLib::Contains(const std::string& name)
{
    return mTextures.find(name) != mTextures.end();
}

bool TextureLib::AddTexture(const std::string& name, std::unique_ptr<Texture> tex)
{
    if(mTextures.find(name) == mTextures.end())
    {
        mTextures[name] = std::move(tex);
        return true;
    }

    return false;
}

Texture* TextureLib::operator[](const std::string& name)
{
    if(mTextures.find(name) != mTextures.end())
    {
        return mTextures[name].get();
    }

    return nullptr;
}

const std::unordered_map<std::string, std::unique_ptr<Texture>>& TextureLib::GetCollection()const
{
    return mTextures;
}