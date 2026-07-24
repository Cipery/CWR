#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/TextureMetal.hpp>

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Textures/LooseTextures.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <cstring>

namespace Poseidon
{
namespace
{
PacFormat BasicFormat(const char* name)
{
    const char* extension = name ? std::strrchr(name, '.') : nullptr;
    return extension && !strcmpi(extension, ".paa") ? PacARGB4444 : PacARGB1555;
}

PacFormat DstFormat(PacFormat source)
{
    switch (source)
    {
        case PacP8:
            return PacARGB1555;
        case PacARGB1555:
        case PacRGB565:
        case PacARGB4444:
        case PacAI88:
        case PacARGB8888:
        case PacDXT1:
        case PacDXT2:
        case PacDXT3:
        case PacDXT4:
        case PacDXT5:
            return source;
        default:
            LOG_WARN(Graphics, "Metal: unsupported source texture format {}", static_cast<int>(source));
            return source;
    }
}
} // namespace

TextureMetal::TextureMetal(TextBankMetal* bank) : _bank(bank) {}

TextureMetal::~TextureMetal()
{
    ReleaseMemory();
    if (GEngine)
        GEngine->TextureDestroyed(this);
}

int TextureMetal::Init(const char* name)
{
    SetName(name);
    _maxSize = 0x10000;
    const RString resolved = Graphics::ResolveLooseTexturePath(name);
    ITextureSourceFactory* factory = SelectTextureSourceFactory(resolved);
    if (!factory || !factory->Check(resolved))
    {
        Foundation::WarningMessage("Cannot load texture %s.", static_cast<const char*>(GetName()));
        return -1;
    }
    return 0;
}

void TextureMetal::DoLoadHeaders()
{
    if (_initialized)
        return;
    _initialized = true;

    PacFormat format = BasicFormat(Name());
    const bool isPaa = format == PacARGB4444;
    if (_maxSize >= 0x10000)
    {
        if (!CmpStartStr(Name(), "fonts\\"))
            _maxSize = 1024;
        else if (!CmpStartStr(Name(), "merged\\"))
            _maxSize = 2048;
        else if (_bank->AnimatedNumber(Name()) >= 0 && IsAlpha())
            _maxSize = ENGINE_CONFIG.maxAnimText;
        else
            _maxSize = ENGINE_CONFIG.maxObjText;
    }

    const RString resolved = Graphics::ResolveLooseTexturePath(Name());
    ITextureSourceFactory* factory = SelectTextureSourceFactory(resolved);
    if (!factory)
        return;
    _src = factory->Create(resolved, _mipmaps, MAX_MIPMAPS);
    if (!_src)
    {
        Foundation::WarningMessage("Cannot load texture %s.", static_cast<const char*>(GetName()));
        return;
    }

    format = _src->GetFormat();
    if (format == PacARGB4444 || format == PacAI88 || format == PacARGB8888)
        _src->ForceAlpha();

    PacFormat destination = DstFormat(format);
    if (!_src->IsTransparent() && format == PacARGB1555 && !isPaa)
        destination = PacRGB565;

    _largestUsed = MAX_MIPMAPS;
    const int sourceMips = _src->GetMipmapCount();
    int i = 0;
    for (; i < sourceMips && i < MAX_MIPMAPS; ++i)
    {
        PacLevelMem& mip = _mipmaps[i];
        mip.SetDestFormat(destination, 8);
        if (!mip.TooLarge(_maxSize) && _largestUsed > i)
            _largestUsed = i;
        if (mip._w < 4 || mip._h < 4)
            break;
    }
    _nMipmaps = i;
    _residentLevel = _nMipmaps;
}

void TextureMetal::LoadHeaders()
{
    DoLoadHeaders();
}

int TextureMetal::AWidth(int level) const
{
    const_cast<TextureMetal*>(this)->DoLoadHeaders();
    return _mipmaps[level]._w;
}

int TextureMetal::AHeight(int level) const
{
    const_cast<TextureMetal*>(this)->DoLoadHeaders();
    return _mipmaps[level]._h;
}

int TextureMetal::ANMipmaps() const
{
    const_cast<TextureMetal*>(this)->DoLoadHeaders();
    return _nMipmaps;
}

AbstractMipmapLevel& TextureMetal::AMipmap(int level)
{
    DoLoadHeaders();
    return _mipmaps[level];
}

const AbstractMipmapLevel& TextureMetal::AMipmap(int level) const
{
    const_cast<TextureMetal*>(this)->DoLoadHeaders();
    return _mipmaps[level];
}

void TextureMetal::ASetNMipmaps(int n)
{
    DoLoadHeaders();
    if (n > _nMipmaps)
        n = _nMipmaps;
    _nMipmaps = n;
    if (_nMipmaps > 0)
    {
        saturateMax(_maxSize, static_cast<int>(_mipmaps[_nMipmaps - 1]._w));
        saturateMax(_maxSize, static_cast<int>(_mipmaps[_nMipmaps - 1]._h));
        saturateMin(_largestUsed, _nMipmaps - 1);
    }
    if (_interpolate)
        _interpolate->ASetNMipmaps(n);
}

void TextureMetal::SetMaxSize(int size)
{
    DoLoadHeaders();
    if (size < _maxSize)
        _maxSize = size;
}

void TextureMetal::SetMipmapRange(int min, int max)
{
    DoLoadHeaders();
    saturateMax(min, 0);
    saturateMin(max, _nMipmaps - 1);
    if (min > max)
        min = max;
    _largestUsed = min;
    _nMipmaps = max + 1;
}

bool TextureMetal::IsAlpha() const
{
    const_cast<TextureMetal*>(this)->DoLoadHeaders();
    return _src && _src->IsAlpha();
}

bool TextureMetal::IsTransparent() const
{
    const_cast<TextureMetal*>(this)->DoLoadHeaders();
    return _src && _src->IsTransparent();
}

Color TextureMetal::GetColor()
{
    DoLoadHeaders();
    return _src ? _src->GetAverageColor() : HBlack;
}

Color TextureMetal::GetPixel(int level, float u, float v) const
{
    TextureMetal* self = const_cast<TextureMetal*>(this);
    self->DoLoadHeaders();
    if (!_src || level < 0 || level >= _nMipmaps)
        return HWhite;

    PacLevelMem mip = _mipmaps[level];
    AUTO_STATIC_ARRAY(char, data, 256 * 256 * 4);
    data.Realloc(mip._pitch * mip._h);
    data.Resize(mip._pitch * mip._h);
    if (!_src->GetMipmapData(data.Data(), mip, level))
        return HWhite;
    Color result = mip.GetPixel(data.Data(), u, v);
    if (_interpolate)
    {
        PacLevelMem otherMip = _interpolate->_mipmaps[level];
        AUTO_STATIC_ARRAY(char, other, 256 * 256 * 4);
        other.Realloc(otherMip._pitch * otherMip._h);
        other.Resize(otherMip._pitch * otherMip._h);
        if (_interpolate->_src->GetMipmapData(other.Data(), otherMip, level))
        {
            Color icol = otherMip.GetPixel(other.Data(), u, v);
            result = result * (1.0f - _iFactor) + icol * _iFactor;
        }
    }
    return result;
}

AlphaStats::Kind TextureMetal::ScanTopMipAlphaClass()
{
    DoLoadHeaders();
    if (!_src)
        return AlphaStats::Opaque;

    QIFStream input;
    GFileServer->Open(input, Name());
    const int size = input.fail() ? 0 : input.rest();
    if (size <= 0)
        return AlphaStats::Opaque;
    AUTO_STATIC_ARRAY(char, fileData, 256 * 1024);
    fileData.Realloc(size);
    fileData.Resize(size);
    input.read(fileData.Data(), size);

    const char* name = Name();
    const std::size_t length = name ? std::strlen(name) : 0;
    const bool isPaa = length >= 4 && (name[length - 1] == 'a' || name[length - 1] == 'A');
    const DecodedImage image = DecodePAABuffer(fileData.Data(), static_cast<std::size_t>(size), isPaa);
    return image.valid() ? ClassifyAlpha(image.rgba.data(), static_cast<std::size_t>(image.width) * image.height).kind
                         : AlphaStats::Opaque;
}

AlphaStats::Kind TextureMetal::GetAlphaClass()
{
    if (_alphaClass >= 0)
        return static_cast<AlphaStats::Kind>(_alphaClass);
    DoLoadHeaders();
    AlphaStats::Kind kind = AlphaStats::Opaque;
    if (_src)
    {
        const bool hasAlpha = _src->IsAlpha();
        const bool chroma = _src->IsTransparent();
        const bool oneBit = _src->GetFormat() == PacDXT1;
        AlphaStats decoded;
        const AlphaStats* decodedPtr = nullptr;
        if (hasAlpha && !oneBit)
        {
            decoded.kind = ScanTopMipAlphaClass();
            decodedPtr = &decoded;
        }
        kind = ClassifyTextureAlpha(hasAlpha, chroma, oneBit, decodedPtr);
    }
    _alphaClass = static_cast<signed char>(kind);
    return kind;
}

bool TextureMetal::IsGpuResident() const
{
    return _bank && _bank->Engine() && _bank->Engine()->IsTextureHandleLive(_handle);
}
} // namespace Poseidon
