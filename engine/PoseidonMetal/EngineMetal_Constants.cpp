#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/Shaders/PoseidonShaderTypes.h>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Core/MatrixConversion.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Poseidon
{
namespace
{
constexpr int kMaxLocalLights = 8;
constexpr int kMaxWorldInstances = 256;
static_assert(sizeof(GfxMatrix) == 64, "WorldInstances matrices must remain 64 bytes");
static_assert(kMaxWorldInstances == EngineMetal::kInstArrayCapacity,
              "upload clamp must match the run-accumulation array capacity");

std::uint64_t LightsSignature(const LightList& lights)
{
    std::uint64_t signature = static_cast<std::uint64_t>(lights.Size());
    for (int i = 0; i < lights.Size(); ++i)
        signature = signature * 1099511628211ull ^ reinterpret_cast<std::uintptr_t>(static_cast<const Light*>(lights[i]));
    return signature;
}
} // namespace

void EngineMetal::MarkConstantsDirty()
{
    _constantsVSDirty = true;
    _constantsPSDirty = true;
    _boundVSBuffer = nullptr;
    _boundPSBuffer = nullptr;
    _boundWorldBuffer = nullptr;
    _encoderVSBuffer = nullptr;
    _encoderPSBuffer = nullptr;
    _encoderWorldBuffer = nullptr;
}

bool EngineMetal::SnapshotConstants()
{
    MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder();
    if (!encoder)
        return false;

    if (_constantsVSDirty)
    {
        FrameRing::Allocation allocation = _frameRing.Allocate(sizeof(VSConstantsPod));
        if (!allocation)
            return false;
        std::memcpy(allocation.contents, _vsConstants.data(), sizeof(VSConstantsPod));
        _boundVSBuffer = allocation.buffer;
        _boundVSOffset = allocation.offset;
        _constantsVSDirty = false;
    }
    if (_constantsPSDirty)
    {
        FrameRing::Allocation allocation = _frameRing.Allocate(sizeof(PSConstantsPod));
        if (!allocation)
            return false;
        std::memcpy(allocation.contents, _psConstants.data(), sizeof(PSConstantsPod));
        _boundPSBuffer = allocation.buffer;
        _boundPSOffset = allocation.offset;
        _constantsPSDirty = false;
    }
    if (!_boundVSBuffer || !_boundPSBuffer)
        return false;
    if (_encoderVSBuffer == _boundVSBuffer)
    {
        if (_encoderVSOffset != _boundVSOffset)
            encoder->setVertexBufferOffset(_boundVSOffset, 0);
    }
    else
    {
        encoder->setVertexBuffer(_boundVSBuffer, _boundVSOffset, 0);
        _encoderVSBuffer = _boundVSBuffer;
    }
    _encoderVSOffset = _boundVSOffset;

    if (_encoderPSBuffer == _boundPSBuffer)
    {
        if (_encoderPSOffset != _boundPSOffset)
            encoder->setFragmentBufferOffset(_boundPSOffset, 1);
    }
    else
    {
        encoder->setFragmentBuffer(_boundPSBuffer, _boundPSOffset, 1);
        _encoderPSBuffer = _boundPSBuffer;
    }
    _encoderPSOffset = _boundPSOffset;
    return true;
}

bool EngineMetal::UploadWorldInstances(const GfxMatrix* matrices, int count)
{
    if (!matrices || count <= 0)
        return true;
    count = std::min(count, kMaxWorldInstances);

    MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder();
    if (!encoder)
        return false;

    const std::size_t bytes = static_cast<std::size_t>(count) * sizeof(GfxMatrix);
    FrameRing::Allocation allocation = _frameRing.Allocate(bytes);
    if (!allocation)
        return false;
    std::memcpy(allocation.contents, matrices, bytes);
    _boundWorldBuffer = allocation.buffer;
    _boundWorldOffset = allocation.offset;
    _runWorldBuffer = allocation.buffer;
    _runWorldOffset = allocation.offset;

    if (_encoderWorldBuffer == _boundWorldBuffer)
    {
        if (_encoderWorldOffset != _boundWorldOffset)
            encoder->setVertexBufferOffset(_boundWorldOffset, 2);
    }
    else
    {
        encoder->setVertexBuffer(_boundWorldBuffer, _boundWorldOffset, 2);
        _encoderWorldBuffer = _boundWorldBuffer;
    }
    _encoderWorldOffset = _boundWorldOffset;
    return true;
}

bool EngineMetal::BindWorldSlot(const GfxMatrix& world)
{
    MTL::RenderCommandEncoder* encoder = EnsureFrameEncoder();
    if (!encoder)
        return false;

    if (_instCount > 1)
    {
        // UploadWorldInstances owns buffer(2) for the complete run. Every
        // section draw must preserve the exact pair so instance_id > 0 never
        // indexes beyond a scalar 64-byte allocation.
        PoseidonAssert(_runWorldBuffer != nullptr);
        PoseidonAssert(_boundWorldBuffer == _runWorldBuffer);
        PoseidonAssert(_boundWorldOffset == _runWorldOffset);
        if (!_runWorldBuffer || _boundWorldBuffer != _runWorldBuffer || _boundWorldOffset != _runWorldOffset)
            return false;
    }
    else
    {
        FrameRing::Allocation allocation = _frameRing.Allocate(sizeof(GfxMatrix));
        if (!allocation)
            return false;
        std::memcpy(allocation.contents, &world, sizeof(GfxMatrix));
        _boundWorldBuffer = allocation.buffer;
        _boundWorldOffset = allocation.offset;
    }

    if (_encoderWorldBuffer == _boundWorldBuffer)
    {
        if (_encoderWorldOffset != _boundWorldOffset)
            encoder->setVertexBufferOffset(_boundWorldOffset, 2);
    }
    else
    {
        encoder->setVertexBuffer(_boundWorldBuffer, _boundWorldOffset, 2);
        _encoderWorldBuffer = _boundWorldBuffer;
    }
    _encoderWorldOffset = _boundWorldOffset;
    PoseidonAssert(_instCount <= 1 || _encoderWorldBuffer == _runWorldBuffer);
    PoseidonAssert(_instCount <= 1 || _encoderWorldOffset == _runWorldOffset);
    return true;
}

void EngineMetal::UploadPSConstant(int slot, const float data[4])
{
    if (std::memcmp(_psConstants.data() + slot * 4, data, 4 * sizeof(float)) == 0)
        return;
    std::memcpy(_psConstants.data() + slot * 4, data, 4 * sizeof(float));
    _constantsPSDirty = true;
}

void EngineMetal::UploadProjection(const GfxMatrix& projection)
{
    std::memcpy(_vsConstants.data() + PoseidonVSSlotProjection * 4, &projection, sizeof(GfxMatrix));
    _constantsVSDirty = true;
}

void EngineMetal::UploadFrameConstants(const FrameState& frame)
{
    std::memcpy(_vsConstants.data() + PoseidonVSSlotProjection * 4, &frame.projection, sizeof(GfxMatrix));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotView * 4, &frame.view, sizeof(GfxMatrix));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotSunDirection * 4, frame.sunDir, 4 * sizeof(float));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotFog * 4, frame.fogParams, 4 * sizeof(float));
    const float camera[4] = {0, 0, 0, 0};
    const float sun[4] = {frame.sunEnabled ? 1.0f : 0.0f, 0, 0, 0};
    const float texControl[4] = {0, 0, 0, 0};
    std::memcpy(_vsConstants.data() + PoseidonVSSlotCamera * 4, camera, sizeof(camera));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotSunEnabled * 4, sun, sizeof(sun));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotTexControl * 4, texControl, sizeof(texControl));
    _constantsVSDirty = true;

    const float fog[4] = {frame.fogColor[0], frame.fogColor[1], frame.fogColor[2], frame.fogColor[3]};
    UploadPSConstant(PoseidonPSSlotFogColor, fog);
}

FrameState EngineMetal::BuildFrameState()
{
    FrameState frame = {};
    if (!GScene || !GScene->GetCamera() || !GScene->MainLight())
        return frame;
    Camera* camera = GScene->GetCamera();
    LightSun* sun = GScene->MainLight();

    ConvertMatrix(frame.view, camera->InverseScaled());
    frame.view._41 = frame.view._42 = frame.view._43 = 0;
    ConvertProjectionMatrix(frame.projection, camera->ProjectionNormal(), _bias);

    const Vector3 cameraPos = camera->Position();
    frame.cameraPos[0] = cameraPos.X();
    frame.cameraPos[1] = cameraPos.Y();
    frame.cameraPos[2] = cameraPos.Z();
    frame.viewport[0] = static_cast<float>(_currentViewport.x);
    frame.viewport[1] = static_cast<float>(_currentViewport.y);
    frame.viewport[2] = static_cast<float>(_currentViewport.width);
    frame.viewport[3] = static_cast<float>(_currentViewport.height);

    const float fogStart = GScene->GetFogMinRange();
    const float fogEnd = GScene->GetFogMaxRange();
    frame.fogParams[0] = fogStart;
    frame.fogParams[1] = fogEnd > fogStart ? 1.0f / (fogEnd - fogStart) : 0.0f;
    frame.fogParams[2] = 1.0f;
    frame.fogColor[0] = _fogColor.R();
    frame.fogColor[1] = _fogColor.G();
    frame.fogColor[2] = _fogColor.B();
    frame.fogColor[3] = 1.0f;

    const Vector3 sunDirection = sun->Direction();
    frame.sunDir[0] = sunDirection.X();
    frame.sunDir[1] = sunDirection.Y();
    frame.sunDir[2] = sunDirection.Z();
    frame.sunEnabled = _sunEnabled;
    return frame;
}

void EngineMetal::UploadMaterialConstants(const TLMaterial& material)
{
    if (!GScene || !GScene->MainLight())
        return;
    LightSun* sun = GScene->MainLight();
    const Color diffuseColor = sun->Diffuse() * material.diffuse;
    const Color ambientColor = sun->Ambient() * material.ambient + sun->Diffuse() * material.forcedDiffuse;
    const Color specularColor = sun->Diffuse() * material.specular;
    const float ambient[4] = {ambientColor.R(), ambientColor.G(), ambientColor.B(), ambientColor.A()};
    const float diffuse[4] = {diffuseColor.R(), diffuseColor.G(), diffuseColor.B(), diffuseColor.A()};
    const float emissive[4] = {
        material.emmisive.R(), material.emmisive.G(), material.emmisive.B(), material.emmisive.A()};
    const float specular[4] = {
        specularColor.R(), specularColor.G(), specularColor.B(), static_cast<float>(material.specularPower)};
    const float specularEnabled[4] = {material.specularPower > 0 ? 1.0f : 0.0f, 0, 0, 0};
    std::memcpy(_vsConstants.data() + PoseidonVSSlotAmbient * 4, ambient, sizeof(ambient));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotDiffuse * 4, diffuse, sizeof(diffuse));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotEmissive * 4, emissive, sizeof(emissive));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotSpecular * 4, specular, sizeof(specular));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotSpecularEnabled * 4, specularEnabled, sizeof(specularEnabled));
    _constantsVSDirty = true;
}

void EngineMetal::UploadLightConstants(const LightList& lights, const TLMaterial& material, float nightEffect)
{
    int count = 0;
    if (nightEffect > 0.0f)
    {
        const Color materialDiffuse = material.diffuse * nightEffect;
        const Color materialAmbient = material.ambient * nightEffect;
        for (int i = 0; i < lights.Size() && count < kMaxLocalLights; ++i)
        {
            Light* light = lights[i];
            if (!light)
                continue;
            LightDescription description;
            light->GetDescription(description);
            const bool spot = description.type == LTSpotLight;
            if (description.type != LTPoint && !spot)
                continue;

            float* position = _vsConstants.data() + (PoseidonVSSlotLightPosition + count) * 4;
            position[0] = description.pos.X() - _frameState.cameraPos[0];
            position[1] = description.pos.Y() - _frameState.cameraPos[1];
            position[2] = description.pos.Z() - _frameState.cameraPos[2];
            position[3] = description.startAtten;

            Vector3 beam = description.dir;
            beam.Normalize();
            float* direction = _vsConstants.data() + (PoseidonVSSlotLightDirection + count) * 4;
            direction[0] = beam.X();
            direction[1] = beam.Y();
            direction[2] = beam.Z();
            direction[3] = spot ? 1.0f : 0.0f;

            const Color diffuse = description.diffuse * materialDiffuse;
            float* localDiffuse = _vsConstants.data() + (PoseidonVSSlotLightDiffuse + count) * 4;
            localDiffuse[0] = diffuse.R();
            localDiffuse[1] = diffuse.G();
            localDiffuse[2] = diffuse.B();
            localDiffuse[3] = 0;

            const Color ambient = description.ambient * materialAmbient;
            float* localAmbient = _vsConstants.data() + (PoseidonVSSlotLightAmbient + count) * 4;
            localAmbient[0] = ambient.R();
            localAmbient[1] = ambient.G();
            localAmbient[2] = ambient.B();
            localAmbient[3] = 0;
            ++count;
        }
    }
    _vsConstants[PoseidonVSSlotLightCount * 4] = static_cast<float>(count);
    _constantsVSDirty = true;
}

void EngineMetal::UploadTexGenConstants(render::TexGenMode mode)
{
    // Water matrices include the current wave phase, so an unchanged mode
    // still needs fresh constants when another water draw is prepared.
    if (_texGenMode == mode && mode != render::TexGenMode::Water)
        return;
    _texGenMode = mode;
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float matrix0[16];
    float matrix1[16];
    std::memcpy(matrix0, identity, sizeof(matrix0));
    std::memcpy(matrix1, identity, sizeof(matrix1));
    float control[4] = {0, 0, 0, 0};
    if (mode == render::TexGenMode::Detail || mode == render::TexGenMode::Grass)
    {
        control[1] = 1;
        matrix1[0] = matrix1[5] = matrix1[10] = 32;
    }
    else if (mode == render::TexGenMode::Water)
    {
        control[0] = control[1] = 1;
        matrix1[0] = matrix1[5] = matrix1[10] = 64;
        const float time = Glob.time.toFloat();
        const float wave1 = std::sin(time * 0.04f);
        const float wave2 = fastFmod(time * 0.3f + std::sin(time * 0.5f) * 0.5f, 2.0f);
        matrix0[8] = wave1 * 0.5f;
        matrix0[9] = wave1;
        matrix1[8] = wave2 * 0.5f;
        matrix1[9] = wave2;
    }
    std::memcpy(_vsConstants.data() + PoseidonVSSlotTexMatrix0 * 4, matrix0, sizeof(matrix0));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotTexMatrix1 * 4, matrix1, sizeof(matrix1));
    std::memcpy(_vsConstants.data() + PoseidonVSSlotTexControl * 4, control, sizeof(control));
    _constantsVSDirty = true;
}

void EngineMetal::SetMaterial(const TLMaterial& material, const LightList& lights, const render::LegacySpec& spec)
{
    const int key = static_cast<int>(static_cast<std::uint32_t>(spec.material & render::Material::DisableSun));
    const std::uint64_t lightSignature = LightsSignature(lights);
    if (material == _materialSet && key == _materialSetSpec && lightSignature == _materialLightsSignature)
        return;
    _materialSet = material;
    _materialSetSpec = key;
    _materialLightsSignature = lightSignature;
    UploadMaterialConstants(material);

    float night = GScene && GScene->MainLight() ? GScene->MainLight()->NightEffect() : 0.0f;
    if (render::Has(spec.material, render::Material::DisableSun))
        night = 1.0f;
    UploadLightConstants(lights, material, night);
}

void EngineMetal::EnableSunLight(bool enable)
{
    if (_sunEnabled == enable)
        return;
    _sunEnabled = enable;
    _frameState.sunEnabled = enable;
    const float value[4] = {enable ? 1.0f : 0.0f, 0, 0, 0};
    std::memcpy(_vsConstants.data() + PoseidonVSSlotSunEnabled * 4, value, sizeof(value));
    _constantsVSDirty = true;
    _materialSetSpec = -1;
}
} // namespace Poseidon
