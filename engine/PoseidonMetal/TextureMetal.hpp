#pragma once

#include <Poseidon/Foundation/Memory/MemFreeReq.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <PoseidonMetal/MetalFwd.hpp>

#include <cstddef>
#include <cstdint>
#include <list>

namespace Poseidon
{
class EngineMetal;
class TextBankMetal;

PacFormat UploadFormatForTextureMetal(PacFormat format, bool interpolate);

class TextureMetal final : public Texture
{
    friend class TextBankMetal;
    friend class EngineMetal;

  public:
    TextureMetal(TextBankMetal* bank);
    ~TextureMetal() override;

    int Init(const char* name);
    bool InitFromRGBA(int w, int h, const void* rgba, std::uint32_t size, bool mipmap);
    void UpdateRGBA(const void* rgba, std::uint32_t size);
    bool EnsureResident(int levelMin);
    void ReleaseMemory();

    void LoadHeaders() override;
    void SetMaxSize(int size) override;
    int AMaxSize() const override { return _maxSize; }
    int AWidth(int level = 0) const override;
    int AHeight(int level = 0) const override;
    int ANMipmaps() const override;
    AbstractMipmapLevel& AMipmap(int level) override;
    const AbstractMipmapLevel& AMipmap(int level) const override;
    void ASetNMipmaps(int n) override;
    Color GetPixel(int level, float u, float v) const override;
    bool IsTransparent() const override;
    Color GetColor() override;
    bool IsAlpha() const override;
    AlphaStats::Kind GetAlphaClass() override;
    bool IsGpuResident() const override;
    bool VerifyChecksum(const MipInfo&) const override { return true; }
    void SetMipmapRange(int min, int max) override;
    void SetMultitexturing(int type) override { _useDetail = type != 0; }

    std::uint32_t GetHandle() const { return _handle; }

  private:
    void DoLoadHeaders();
    AlphaStats::Kind ScanTopMipAlphaClass();

    TextBankMetal* _bank = nullptr;
    SRef<ITextureSource> _src;
    Ref<TextureMetal> _interpolate;
    float _iFactor = 0.0f;
    int _maxSize = 256;
    int _nMipmaps = 0;
    int _residentLevel = MAX_MIPMAPS;
    int _largestUsed = MAX_MIPMAPS;
    int _levelNeededThisFrame = MAX_MIPMAPS;
    int _levelNeededLastFrame = MAX_MIPMAPS;
    std::size_t _allocatedBytes = 0;
    std::uint64_t _lastUseSerial = 0;
    std::uint64_t _lastUseFrame = 0;
    bool _wholeUse = false;
    std::list<TextureMetal*>::iterator _lruIt;
    bool _inLru = false;
    PacLevelMem _mipmaps[MAX_MIPMAPS];
    MTL::Texture* _surface = nullptr; // Owned.
    std::uint32_t _handle = 0;
    signed char _alphaClass = -1;
    bool _initialized = false;
    bool _dynamic = false;
    bool _dynamicMipmapped = false;
    bool _useDetail = false;
};

class TextBankMetal final : public AbstractTextBank
{
    friend class TextureMetal;
    friend class EngineMetal;

  public:
    explicit TextBankMetal(EngineMetal* engine);
    ~TextBankMetal() override;

    Ref<Texture> Load(RStringB name) override;
    Ref<Texture> LoadInterpolated(RStringB n1, RStringB n2, float factor) override;
    int NTextures() const override { return _textures.Size(); }
    Texture* GetTexture(int i) const override { return _textures[i]; }
    MipInfo UseMipmap(Texture* texture, int level, int levelTop) override;
    void Compact() override { _textures.Compact(); }
    void Preload() override;
    void FlushTextures() override;
    void ForceReloadAll() override;
    void ReleaseAllTextures() override;
    void FlushBank(QFBank* bank) override;
    Texture* CreateDynamic(int w, int h, const void* rgba, std::uint32_t size, bool mipmap = false) override;
    void UpdateDynamic(Texture* texture, const void* rgba, std::uint32_t size) override;
    void StartFrame() override;
    void FinishFrame() override;
    void BoostLoadBudget(int frames) override;

    TextureMetal* GetDetailTexture() const { return _detail; }
    TextureMetal* GetGrassTexture() const { return _grass; }
    TextureMetal* GetSpecularTexture() const { return _specular; }
    TextureMetal* GetWaterBumpMap() const { return _waterBump; }

    EngineMetal* Engine() const { return _engine; }
    void OnResident(TextureMetal& texture, std::size_t bytes, int level, bool allocated);
    void OnReleased(TextureMetal& texture);

  private:
    int Find(RStringB name, TextureMetal* interpolate = nullptr) const;
    int FindFree() const;
    TextureMetal* Copy(int from);
    void InitDetailTextures();
    void CheckTextureMemory();
    bool ReserveMemory(std::size_t bytes, TextureMetal* protectedTexture = nullptr);
    bool ForcedReserveMemory(std::size_t bytes, TextureMetal* protectedTexture = nullptr);
    void Touch(TextureMetal& texture);
    std::size_t TextureBudget() const;

    EngineMetal* _engine = nullptr;
    LLinkArray<TextureMetal> _textures;
    Ref<TextureMetal> _detail;
    Ref<TextureMetal> _specular;
    Ref<TextureMetal> _grass;
    Ref<TextureMetal> _waterBump;
    std::list<TextureMetal*> _lru;
    std::size_t _totalAllocated = 0;
    std::size_t _maxTextureMemory = 0;
    std::size_t _limitAllocatedTextures = 0;
    std::size_t _maxSmallTexturePixels = 16;
    std::uint64_t _useSerial = 0;
    std::uint64_t _frameSerial = 0;
    int _thisFrameAllocations = 0;
    int _loadBoostFrames = 0;
    Foundation::MemoryDomainProbe _memProbe;
};
} // namespace Poseidon
