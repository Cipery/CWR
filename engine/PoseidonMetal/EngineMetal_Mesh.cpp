#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/Shaders/PoseidonShaderTypes.h>
#include <PoseidonMetal/TextureMetal.hpp>

#include <Poseidon/Graphics/Core/MatrixConversion.hpp>
#include <Poseidon/Graphics/Core/ZBiasMath.hpp>
#include <Poseidon/Graphics/Rendering/BuildRenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>

#include <cstring>

namespace Poseidon
{
void EngineMetal::BeginInstancedRun(int count)
{
    PoseidonAssert(count >= 0 && count <= static_cast<int>(_instArray.size()));
    _instCount = count;
    _instImpure = false;
}

bool EngineMetal::EndInstancedRun()
{
    const bool pure = !_instImpure;
    _instCount = 0;
    _runWorldBuffer = nullptr;
    _runWorldOffset = 0;
    return pure;
}

bool EngineMetal::InstancedRunAdd(const Matrix4& modelToWorld)
{
    if (_instPending >= static_cast<int>(_instArray.size()))
        return false;
    GfxMatrix& matrix = _instArray[static_cast<std::size_t>(_instPending)];
    ConvertMatrix(matrix, modelToWorld);
    matrix._41 -= _frameState.cameraPos[0];
    matrix._42 -= _frameState.cameraPos[1];
    matrix._43 -= _frameState.cameraPos[2];
    ++_instPending;
    return true;
}

void EngineMetal::BeginInstancedRunUpload()
{
    _runWorldBuffer = nullptr;
    _runWorldOffset = 0;
    const bool uploaded = UploadWorldInstances(_instArray.data(), _instPending);
    BeginInstancedRun(_instPending);
    if (!uploaded && _instPending > 0)
    {
        // Draw the head through the scalar path and make EndInstancedRun ask
        // the caller to redraw the remaining instances scalar.
        _instCount = 0;
        _instImpure = true;
        RecordDiagnostic("failed to upload an instanced-run WorldInstances block");
    }
}

void EngineMetal::PrepareMeshTL(const LightList&, const Matrix4& modelToWorld, const render::LegacySpec& spec)
{
    FlushQueues();
    if (!_in3DPass)
    {
        _in3DPass = true;
        // GL33 starts its comparison stream at BeginPass; discard any
        // screen-space captures that preceded this first world draw.
        _drawItems.clear();
        _frameState = BuildFrameState();
        UploadFrameConstants(_frameState);
        ApplyWorldViewport();
    }
    _activePassId = static_cast<int>(SpecToPassId(spec));
    EnableSunLight(!render::Has(spec.material, render::Material::DisableSun));

    GfxMatrix world;
    ConvertMatrix(world, modelToWorld);
    world._41 -= _frameState.cameraPos[0];
    world._42 -= _frameState.cameraPos[1];
    world._43 -= _frameState.cameraPos[2];
    _currentDrawItem = {};
    _currentDrawItem.worldMatrix = world;
    _currentDrawItem.specFlags = spec;
    _currentDrawItem.bias = _bias;

    float constantColor[4] = {1, 1, 1, 1};
    if (GScene && render::Has(spec.routing, render::Routing::IsColored))
    {
        const ColorVal color = GScene->GetConstantColor();
        constantColor[0] = color.R();
        constantColor[1] = color.G();
        constantColor[2] = color.B();
        constantColor[3] = color.A();
    }
    UploadPSConstant(PoseidonPSSlotConstantColor, constantColor);
}

void EngineMetal::PrepareTriangleTL(const MipInfo& mip, const render::LegacySpec& spec)
{
    TextureMetal* texture = static_cast<TextureMetal*>(mip._texture);
    render::BuildContext context;
    context.isIn3DPass = true;
    context.isMultitexturing = IsMultitexturing();
    context.shadowAlphaRef = static_cast<std::uint8_t>((_shadowFactor * 7) >> 4);
    context.passKindHint = GetPassKindHint();
    const render::RenderPassDescriptor descriptor = render::BuildRenderPassDescriptor(spec, context);
    UploadTexGenConstants(descriptor.texGen);
    if (descriptor.shader == render::ShaderFamily::Water && GScene && GScene->MainLight())
    {
        const Vector3 direction = GScene->MainLight()->SunDirection();
        const float lightDirection[4] = {direction.X(), direction.Y(), direction.Z(), 0};
        UploadPSConstant(PoseidonPSSlotLightDirection, lightDirection);
    }

    _currentDrawItem.texture = texture;
    _currentDrawItem.textureLevel = mip._level;
    _currentDrawItem.backendTextureHandle = texture ? texture->GetHandle() : 0;
    _currentDrawItem.backendTexture1Handle = 0;

    TextureMetal* secondary = nullptr;
    if (_textBank)
    {
        if (descriptor.shader == render::ShaderFamily::Grass)
            secondary = _textBank->GetGrassTexture();
        else if (descriptor.shader == render::ShaderFamily::Detail)
        {
            secondary = render::Has(spec.backend, render::Backend::SpecularTexture)
                            ? _textBank->GetSpecularTexture()
                            : _textBank->GetDetailTexture();
        }
        else if (descriptor.shader == render::ShaderFamily::Water)
            secondary = _textBank->GetDetailTexture();
        if (secondary)
            _textBank->UseMipmap(secondary, 0, 0);
    }
    if (secondary)
        _currentDrawItem.backendTexture1Handle = secondary->GetHandle();

    _skipCurrentWorldDraw =
        descriptor.shader == render::ShaderFamily::Shadow || !ApplyWorldState(descriptor);
}

void EngineMetal::UpdateProjection()
{
    if (!_in3DPass || !GScene || !GScene->GetCamera())
        return;
    FlushQueues();
    ConvertProjectionMatrix(_frameState.projection, GScene->GetCamera()->ProjectionNormal(), _bias);
    UploadProjection(_frameState.projection);
}

void EngineMetal::SetBias(int value)
{
    if (_bias == value)
        return;
    _bias = value;
    UpdateProjection();
}

void EngineMetal::GetZCoefs(float& zAdd, float& zMult)
{
    const auto coefficients = render::zbias::SoftwareCoefs(_bias);
    zAdd = coefficients.zAdd;
    zMult = coefficients.zMult;
}

void EngineMetal::FogColorChanged(ColorVal)
{
    _frameState.fogColor[0] = _fogColor.R();
    _frameState.fogColor[1] = _fogColor.G();
    _frameState.fogColor[2] = _fogColor.B();
    _frameState.fogColor[3] = 1.0f;
    UploadPSConstant(PoseidonPSSlotFogColor, _frameState.fogColor);
}

void EngineMetal::EnableNightEye(float night)
{
    if (_nightEye == night)
        return;
    _nightEye = night;
    const float coefficients[4] = {0.299f, 0.587f, 0.114f, 1.0f - night};
    UploadPSConstant(PoseidonPSSlotNightEye, coefficients);
}

void EngineMetal::SetGrassParams(float a1, float a2, float a3, float a4)
{
    const float values[4] = {a1, a2, a3, a4};
    if (std::memcmp(values, _grassParams.data(), sizeof(values)) == 0)
        return;
    std::memcpy(_grassParams.data(), values, sizeof(values));
    const float coefficient1[4] = {0, 0, 0, a1};
    const float coefficient2[4] = {0, 0, 0, a2};
    UploadPSConstant(PoseidonPSSlotGrassCoefficient1, coefficient1);
    UploadPSConstant(PoseidonPSSlotGrassCoefficient2, coefficient2);
}
} // namespace Poseidon
