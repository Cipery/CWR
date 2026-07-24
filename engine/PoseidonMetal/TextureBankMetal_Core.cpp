#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/TextureMetal.hpp>

#include <Poseidon/Dev/Diag/ScopedTimer.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Textures/LooseTextures.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>

#include <algorithm>
#include <cmath>

extern ParamFile Remaster;

namespace Poseidon
{
namespace
{
constexpr std::size_t kFallbackTextureBudget = 256ull * 1024ull * 1024ull;
constexpr std::size_t kMaximumTextureBudget = 512ull * 1024ull * 1024ull;
constexpr int kMaxAllocationsPerFrame = 8;
} // namespace

TextBankMetal::TextBankMetal(EngineMetal* engine) : _engine(engine)
{
    _maxTextureMemory = TextureBudget();
    _limitAllocatedTextures = _maxTextureMemory;

    std::size_t limitPixels = _limitAllocatedTextures / (2 * 1024 * 8);
    std::size_t powerOfTwo = 1;
    while (powerOfTwo <= limitPixels / 2)
        powerOfTwo *= 2;
    _maxSmallTexturePixels = std::max<std::size_t>(powerOfTwo, 16);

    _memProbe.Register(
        "Metal Textures", 0.5f, [this] { return _totalAllocated; }, [this] { return _maxTextureMemory; },
        [this] { return static_cast<std::size_t>(_textures.Size()); });
    LOG_INFO(Graphics, "Metal: texture residency budget {} MB, small fallback limit {} pixels",
             _maxTextureMemory / (1024 * 1024), _maxSmallTexturePixels);
}

TextBankMetal::~TextBankMetal()
{
    UnlockAllTextures();
    DeleteAllAnimated();
    _textures.Compact();
    _textures.Clear();
    _detail.Free();
    _waterBump.Free();
    _specular.Free();
    _grass.Free();
    _lru.clear();
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

    const int minimumPixels = static_cast<int>(_maxSmallTexturePixels / 4);
    for (; level > 0; --level)
    {
        const PacLevelMem& mip = texture->_mipmaps[level];
        if (mip._w * mip._h >= minimumPixels)
            break;
    }
    saturateMin(top, level);

    if (_loadBoostFrames <= 0 && _thisFrameAllocations > kMaxAllocationsPerFrame)
    {
        if (texture->_residentLevel < texture->_nMipmaps)
            top = level = texture->_residentLevel;
        else
        {
            for (top = level; top + 1 < texture->_nMipmaps; ++top)
            {
                const PacLevelMem& mip = texture->_mipmaps[top];
                if (static_cast<std::size_t>(mip._w) * mip._h <= _maxSmallTexturePixels)
                    break;
            }
            level = top;
        }
    }

    if (texture->_levelNeededThisFrame > level)
        texture->_levelNeededThisFrame = level;
    for (int residentTop = top; residentTop < texture->_nMipmaps; ++residentTop)
    {
        if (!texture->EnsureResident(residentTop))
            continue;
        Touch(*texture);
        return MipInfo(texture, texture->_residentLevel);
    }
    return MipInfo(texture, -1);
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

std::size_t TextBankMetal::TextureBudget() const
{
    if (!_engine || !_engine->_metal.device)
        return kFallbackTextureBudget;
    const std::uint64_t recommended = _engine->_metal.device->recommendedMaxWorkingSetSize();
    if (recommended == 0)
    {
        LOG_WARN(Graphics, "Metal: recommendedMaxWorkingSetSize unavailable; using 256 MB texture budget");
        return kFallbackTextureBudget;
    }
    return static_cast<std::size_t>(std::min<std::uint64_t>(recommended, kMaximumTextureBudget));
}

void TextBankMetal::CheckTextureMemory()
{
    _limitAllocatedTextures = _maxTextureMemory;
    ReserveMemory(0);
}

void TextBankMetal::StartFrame()
{
    ++_frameSerial;
    InitDetailTextures();
    CheckTextureMemory();
    _thisFrameAllocations = 0;
    if (_loadBoostFrames > 0)
        --_loadBoostFrames;
}

void TextBankMetal::FinishFrame()
{
    for (TextureMetal* texture : _lru)
    {
        if (!texture)
            continue;
        texture->_levelNeededLastFrame = texture->_levelNeededThisFrame;
        texture->_levelNeededThisFrame = texture->_nMipmaps;
    }
}

void TextBankMetal::BoostLoadBudget(int frames)
{
    if (frames > _loadBoostFrames)
        _loadBoostFrames = frames;
}

void TextBankMetal::Touch(TextureMetal& texture)
{
    texture._lastUseSerial = ++_useSerial;
    texture._lastUseFrame = _frameSerial;
    texture._wholeUse =
        std::min(texture._levelNeededThisFrame, texture._levelNeededLastFrame) <= texture._residentLevel;
    if (texture._inLru)
        _lru.splice(_lru.begin(), _lru, texture._lruIt);
    else
    {
        _lru.push_front(&texture);
        texture._lruIt = _lru.begin();
        texture._inLru = true;
    }
}

bool TextBankMetal::ReserveMemory(std::size_t bytes, TextureMetal* protectedTexture)
{
    while (_totalAllocated + bytes > _limitAllocatedTextures)
    {
        TextureMetal* victim = nullptr;
        for (TextureMetal* candidate : _lru)
        {
            if (!candidate || candidate == protectedTexture || candidate->_dynamic)
                continue;
            // Match GL33's normal reservation tier: current-frame and
            // last-frame residents are protected and callers fall back to a
            // smaller mip. ForcedReserveMemory is the only path allowed to
            // evict either protected tier.
            if (candidate->_lastUseFrame + 1 >= _frameSerial)
                continue;
            if (!victim || candidate->_lastUseFrame < victim->_lastUseFrame ||
                (candidate->_lastUseFrame == victim->_lastUseFrame && !candidate->_wholeUse && victim->_wholeUse) ||
                (candidate->_lastUseFrame == victim->_lastUseFrame && candidate->_wholeUse == victim->_wholeUse &&
                 candidate->_lastUseSerial < victim->_lastUseSerial))
                victim = candidate;
        }
        if (!victim)
            return _totalAllocated + bytes <= _limitAllocatedTextures;
        victim->ReleaseMemory();
    }
    return true;
}

bool TextBankMetal::ForcedReserveMemory(std::size_t bytes, TextureMetal* protectedTexture)
{
    const std::size_t releaseBytes = std::max<std::size_t>(bytes, 4 * 1024);
    const std::size_t target = _totalAllocated > releaseBytes ? _totalAllocated - releaseBytes : 0;
    while (_totalAllocated > target)
    {
        TextureMetal* victim = nullptr;
        for (TextureMetal* candidate : _lru)
        {
            if (!candidate || candidate == protectedTexture || candidate->_dynamic)
                continue;
            if (!victim || candidate->_lastUseFrame < victim->_lastUseFrame ||
                (candidate->_lastUseFrame == victim->_lastUseFrame && !candidate->_wholeUse && victim->_wholeUse) ||
                (candidate->_lastUseFrame == victim->_lastUseFrame && candidate->_wholeUse == victim->_wholeUse &&
                 candidate->_lastUseSerial < victim->_lastUseSerial))
                victim = candidate;
        }
        if (!victim)
            break;
        victim->ReleaseMemory();
    }
    CheckTextureMemory();
    return _totalAllocated <= target;
}

void TextBankMetal::OnResident(TextureMetal& texture, std::size_t bytes, int level, bool allocated)
{
    if (texture._allocatedBytes <= _totalAllocated)
        _totalAllocated -= texture._allocatedBytes;
    else
        _totalAllocated = 0;
    texture._allocatedBytes = bytes;
    texture._residentLevel = level;
    _totalAllocated += bytes;
    if (allocated)
        ++_thisFrameAllocations;
    Touch(texture);
}

void TextBankMetal::OnReleased(TextureMetal& texture)
{
    if (texture._allocatedBytes <= _totalAllocated)
        _totalAllocated -= texture._allocatedBytes;
    else
        _totalAllocated = 0;
    texture._allocatedBytes = 0;
    if (texture._inLru)
    {
        _lru.erase(texture._lruIt);
        texture._inLru = false;
    }
}

void TextBankMetal::InitDetailTextures()
{
    if (_detail)
        return;

    const ParamEntry& names = Remaster >> "CfgDetailTextures";
    auto loadSpecial = [&](Ref<TextureMetal>& destination, RStringB name, int maxSize)
    {
        if (!QIFStreamB::FileExist(name))
            return;
        destination = new TextureMetal(this);
        if (destination->Init(name) != 0)
        {
            destination.Free();
            return;
        }
        destination->SetMaxSize(maxSize);
    };
    loadSpecial(_detail, names >> "detail", 256);
    loadSpecial(_specular, names >> "specular", 256);
    loadSpecial(_grass, names >> "grass", 1024);
    loadSpecial(_waterBump, names >> "waterBump", 1024);
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
        texture->_levelNeededThisFrame = MAX_MIPMAPS;
        texture->_levelNeededLastFrame = MAX_MIPMAPS;
        texture->_alphaClass = -1;
    }
}

void TextBankMetal::ReleaseAllTextures()
{
    for (int i = 0; i < _textures.Size(); ++i)
        if (_textures[i])
            _textures[i]->ReleaseMemory();
    if (_detail)
        _detail->ReleaseMemory();
    if (_specular)
        _specular->ReleaseMemory();
    if (_grass)
        _grass->ReleaseMemory();
    if (_waterBump)
        _waterBump->ReleaseMemory();
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
