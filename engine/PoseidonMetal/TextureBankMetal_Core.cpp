#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/TextureMetal.hpp>

#include <Poseidon/Dev/Diag/ScopedTimer.hpp>
#include <Poseidon/Graphics/Textures/LooseTextures.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <cmath>

namespace Poseidon
{
TextBankMetal::TextBankMetal(EngineMetal* engine) : _engine(engine) {}

TextBankMetal::~TextBankMetal()
{
    UnlockAllTextures();
    DeleteAllAnimated();
    _textures.Compact();
    _textures.Clear();
}

int TextBankMetal::Find(RStringB name, TextureMetal* interpolate) const
{
    for (int i = 0; i < _textures.Size(); ++i)
    {
        TextureMetal* texture = _textures[i];
        if (texture && texture->GetName() == name && texture->_interpolate == interpolate)
            return i;
    }
    return -1;
}

int TextBankMetal::FindFree() const
{
    for (int i = 0; i < _textures.Size(); ++i)
        if (!_textures[i])
            return i;
    return _textures.Size();
}

TextureMetal* TextBankMetal::Copy(int from)
{
    if (from < 0 || from >= _textures.Size() || !_textures[from])
        return nullptr;
    const int index = FindFree();
    TextureMetal* texture = new TextureMetal(this);
    if (texture->Init(_textures[from]->Name()) != 0)
    {
        delete texture;
        return nullptr;
    }
    _textures.Access(index);
    _textures[index] = texture;
    return texture;
}

Ref<Texture> TextBankMetal::Load(RStringB name)
{
    const int existing = Find(name);
    if (existing >= 0)
        return _textures[existing].GetLink();

    const RString resolved = Graphics::ResolveLooseTexturePath(name);
    if (!QIFStreamB::FileExist(resolved))
    {
        const char* value = static_cast<const char*>(name);
        if (value && value[0] && value[0] != '#')
            LOG_WARN(Graphics, "Metal: Cannot load texture {}", value);
        return nullptr;
    }

    const auto start = Dev::Perf::Now();
    Ref<TextureMetal> texture = new TextureMetal(this);
    if (!texture || texture->Init(name) != 0)
        return nullptr;
    const int index = FindFree();
    _textures.Access(index);
    _textures[index] = texture.GetRef();
    Dev::Perf::EmitTraceEventAsset(Foundation::LogCategory::Graphics, "TextBankMetal::Load", start,
                                   static_cast<const char*>(name));
    return texture.GetRef();
}

Ref<Texture> TextBankMetal::LoadInterpolated(RStringB n1, RStringB n2, float factor)
{
    constexpr float epsilon = 1.0f / 256.0f;
    if (factor >= 1.0f - epsilon)
        return Load(n2);
    if (factor <= epsilon)
        return Load(n1);

    Ref<Texture> secondBase = Load(n2);
    TextureMetal* second = static_cast<TextureMetal*>(secondBase.GetRef());
    const int existing = Find(n1, second);
    if (existing >= 0)
    {
        TextureMetal* texture = _textures[existing];
        if (std::fabs(texture->_iFactor - factor) > 1.0f / 64.0f)
        {
            texture->ReleaseMemory();
            texture->_iFactor = factor;
        }
        return texture;
    }

    Load(n1);
    Ref<TextureMetal> texture = Copy(Find(n1));
    if (texture)
    {
        texture->_interpolate = second;
        texture->_iFactor = factor;
    }
    return texture.GetRef();
}

// TODO(M2) — ENTRY GATE: port TextureBankGL33_Cache.cpp's frame LRU,
// VRAM-budget eviction, resident mip-range decisions, and per-frame
// allocation/copy throttles before static-world rendering is enabled. The M1
// bank below intentionally provides demand loading only; it is not a complete
// residency policy for world-scale texture sets.
MipInfo TextBankMetal::UseMipmap(Texture* baseTexture, int level, int top)
{
    if (!baseTexture)
        return MipInfo(nullptr, 0);
    TextureMetal* texture = static_cast<TextureMetal*>(baseTexture);
    texture->LoadHeaders();
    if (texture->_dynamic)
        return MipInfo(texture, 0);
    if (texture->_nMipmaps <= 0)
        return MipInfo(texture, -1);

    saturateMin(level, texture->_mipmapNeeded);
    saturateMin(top, texture->_mipmapWanted);
    saturateMax(level, 0);
    saturateMin(level, texture->_nMipmaps - 1);
    saturateMax(top, texture->_largestUsed);
    saturateMin(top, level);
    saturateMax(level, top);
    if (!texture->EnsureResident(top))
        return MipInfo(texture, -1);
    return MipInfo(texture, texture->_residentLevel);
}

Texture* TextBankMetal::CreateDynamic(int w, int h, const void* rgba, std::uint32_t size, bool mipmap)
{
    const int index = FindFree();
    TextureMetal* texture = new TextureMetal(this);
    _textures.Access(index);
    _textures[index] = texture;
    if (!texture->InitFromRGBA(w, h, rgba, size, mipmap))
    {
        _textures.Delete(index);
        return nullptr;
    }
    return texture;
}

void TextBankMetal::UpdateDynamic(Texture* texture, const void* rgba, std::uint32_t size)
{
    if (texture && rgba)
        static_cast<TextureMetal*>(texture)->UpdateRGBA(rgba, size);
}

void TextBankMetal::Preload()
{
    Compact();
    for (int i = 0; i < _textures.Size(); ++i)
        if (_textures[i])
            _textures[i]->LoadHeaders();
}

void TextBankMetal::FlushTextures()
{
    Compact();
}

void TextBankMetal::ForceReloadAll()
{
    for (int i = 0; i < _textures.Size(); ++i)
    {
        TextureMetal* texture = _textures[i];
        if (!texture || texture->_dynamic)
            continue;
        texture->ReleaseMemory();
        texture->_src = nullptr;
        texture->_initialized = false;
        texture->_residentLevel = MAX_MIPMAPS;
        texture->_alphaClass = -1;
    }
}

void TextBankMetal::ReleaseAllTextures()
{
    for (int i = 0; i < _textures.Size(); ++i)
        if (_textures[i])
            _textures[i]->ReleaseMemory();
}

void TextBankMetal::FlushBank(QFBank* bank)
{
    for (int i = 0; i < _textures.Size(); ++i)
    {
        TextureMetal* texture = _textures[i];
        if (texture && bank->FileExists(texture->GetName()))
        {
            _textures.Delete(i);
            --i;
        }
    }
}
} // namespace Poseidon
