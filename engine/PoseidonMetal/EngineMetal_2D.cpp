#include <PoseidonMetal/Private/MetalCppFirst.hpp>

#include <PoseidonMetal/EngineMetal.hpp>
#include <PoseidonMetal/TextureMetal.hpp>

#include <Poseidon/Graphics/Rendering/Primitives/Clip2D.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/World/Scene/Scene.hpp>

#include <utility>

namespace Poseidon
{
void EngineMetal::Draw2D(const Draw2DPars& pars, const Rect2DAbs& rect, const Rect2DAbs& clip)
{
    if (!pars.mip.IsOK())
        return;

    float xBegin = rect.x;
    float xEnd = rect.x + rect.w;
    float yBegin = rect.y;
    float yEnd = rect.y + rect.h;
    float uBegin = 0.0f, uEnd = 1.0f;
    float vBegin = 0.0f, vEnd = 1.0f;
    const float clipX = floatMax(clip.x, 0);
    const float clipY = floatMax(clip.y, 0);
    const float clipEndX = floatMin(clip.x + clip.w, _w);
    const float clipEndY = floatMin(clip.y + clip.h, _h);

    if (xBegin < clipX)
    {
        uBegin = (clipX - xBegin) / rect.w;
        xBegin = clipX;
    }
    if (xEnd > clipEndX)
    {
        uEnd = 1.0f - (xEnd - clipEndX) / rect.w;
        xEnd = clipEndX;
    }
    if (yBegin < clipY)
    {
        vBegin = (clipY - yBegin) / rect.h;
        yBegin = clipY;
    }
    if (yEnd > clipEndY)
    {
        vEnd = 1.0f - (yEnd - clipEndY) / rect.h;
        yEnd = clipEndY;
    }
    if (xBegin >= xEnd || yBegin >= yEnd)
        return;

    TLVertex vertices[4] = {};
    for (TLVertex& vertex : vertices)
    {
        vertex.rhw = 1.0f;
        vertex.pos[2] = 0.5f;
        vertex.specular = PackedColor(0xff000000);
    }
    vertices[0].color = pars.colorTL;
    vertices[1].color = pars.colorTR;
    vertices[2].color = pars.colorBR;
    vertices[3].color = pars.colorBL;

    const float uTL = pars.uTL + uBegin * (pars.uTR - pars.uTL) + vBegin * (pars.uBL - pars.uTL);
    const float uTR = pars.uTL + uEnd * (pars.uTR - pars.uTL) + vBegin * (pars.uBL - pars.uTL);
    const float uBL = pars.uTL + uBegin * (pars.uTR - pars.uTL) + vEnd * (pars.uBL - pars.uTL);
    const float uBR = pars.uTL + uEnd * (pars.uTR - pars.uTL) + vEnd * (pars.uBL - pars.uTL);
    const float vTL = pars.vTL + uBegin * (pars.vTR - pars.vTL) + vBegin * (pars.vBL - pars.vTL);
    const float vTR = pars.vTL + uEnd * (pars.vTR - pars.vTL) + vBegin * (pars.vBL - pars.vTL);
    const float vBL = pars.vTL + uBegin * (pars.vTR - pars.vTL) + vEnd * (pars.vBL - pars.vTL);
    const float vBR = pars.vTL + uEnd * (pars.vTR - pars.vTL) + vEnd * (pars.vBL - pars.vTL);

    vertices[0].pos[0] = xBegin;
    vertices[0].pos[1] = yBegin;
    vertices[1].pos[0] = xEnd;
    vertices[1].pos[1] = yBegin;
    vertices[2].pos[0] = xEnd;
    vertices[2].pos[1] = yEnd;
    vertices[3].pos[0] = xBegin;
    vertices[3].pos[1] = yEnd;
    vertices[0].t0 = {uTL, vTL};
    vertices[1].t0 = {uTR, vTR};
    vertices[2].t0 = {uBR, vBR};
    vertices[3].t0 = {uBL, vBL};

    QueueVertices(vertices, 4);
    QueueFor(static_cast<TextureMetal*>(pars.mip._texture), pars.mip._level, pars.spec);
    Queue2DPoly(4);
}

void EngineMetal::DrawPoly(const MipInfo& mip, const Vertex2DPixel* input, int count, const Rect2DPixel& clip,
                           int spec)
{
    constexpr int maxVertices = 32;
    if (!input || count < 3)
        return;
    saturateMin(count, maxVertices);

    ClipFlags any = 0;
    ClipFlags all = ClipAll;
    for (int i = 0; i < count; ++i)
    {
        ClipFlags flags = 0;
        if (input[i].x < clip.x)
            flags |= ClipLeft;
        else if (input[i].x > clip.x + clip.w)
            flags |= ClipRight;
        if (input[i].y < clip.y)
            flags |= ClipTop;
        else if (input[i].y > clip.y + clip.h)
            flags |= ClipBottom;
        any |= flags;
        all &= flags;
    }
    if (all)
        return;

    Vertex2DPixel scratchA[maxVertices];
    Vertex2DPixel scratchB[maxVertices];
    if (any)
    {
        Vertex2DPixel* free = scratchA;
        Vertex2DPixel* used = scratchB;
        for (int i = 0; i < count; ++i)
            used[i] = input[i];
        if (any & ClipTop)
            count = Clip2D(clip, free, used, count, InsideTopPixel), std::swap(free, used);
        if (any & ClipBottom)
            count = Clip2D(clip, free, used, count, InsideBottomPixel), std::swap(free, used);
        if (any & ClipLeft)
            count = Clip2D(clip, free, used, count, InsideLeftPixel), std::swap(free, used);
        if (any & ClipRight)
            count = Clip2D(clip, free, used, count, InsideRightPixel), std::swap(free, used);
        if (count < 3)
            return;
        input = used;
    }

    TLVertex vertices[maxVertices] = {};
    const float xOffset = Left2D();
    const float yOffset = Top2D();
    for (int i = 0; i < count; ++i)
    {
        vertices[i].pos[0] = input[i].x + xOffset;
        vertices[i].pos[1] = input[i].y + yOffset;
        vertices[i].pos[2] = input[i].z;
        vertices[i].rhw = input[i].w;
        vertices[i].color = input[i].color;
        vertices[i].specular = PackedColor(0xff000000);
        vertices[i].t0 = {input[i].u, input[i].v};
    }
    QueueVertices(vertices, count);
    QueueFor(static_cast<TextureMetal*>(mip._texture), mip._level, spec);
    Queue2DPoly(count);
}

void EngineMetal::DrawPoly(const MipInfo& mip, const Vertex2DAbs* input, int count, const Rect2DAbs& clip, int spec)
{
    constexpr int maxVertices = 32;
    if (!input || count < 3)
        return;
    saturateMin(count, maxVertices);

    ClipFlags any = 0;
    ClipFlags all = ClipAll;
    for (int i = 0; i < count; ++i)
    {
        ClipFlags flags = 0;
        if (input[i].x < clip.x)
            flags |= ClipLeft;
        else if (input[i].x > clip.x + clip.w)
            flags |= ClipRight;
        if (input[i].y < clip.y)
            flags |= ClipTop;
        else if (input[i].y > clip.y + clip.h)
            flags |= ClipBottom;
        any |= flags;
        all &= flags;
    }
    if (all)
        return;

    Vertex2DAbs scratchA[maxVertices];
    Vertex2DAbs scratchB[maxVertices];
    if (any)
    {
        Vertex2DAbs* free = scratchA;
        Vertex2DAbs* used = scratchB;
        for (int i = 0; i < count; ++i)
            used[i] = input[i];
        if (any & ClipTop)
            count = Clip2D(clip, free, used, count, InsideTopAbs), std::swap(free, used);
        if (any & ClipBottom)
            count = Clip2D(clip, free, used, count, InsideBottomAbs), std::swap(free, used);
        if (any & ClipLeft)
            count = Clip2D(clip, free, used, count, InsideLeftAbs), std::swap(free, used);
        if (any & ClipRight)
            count = Clip2D(clip, free, used, count, InsideRightAbs), std::swap(free, used);
        if (count < 3)
            return;
        input = used;
    }

    TLVertex vertices[maxVertices] = {};
    for (int i = 0; i < count; ++i)
    {
        vertices[i].pos[0] = input[i].x;
        vertices[i].pos[1] = input[i].y;
        vertices[i].pos[2] = input[i].z;
        vertices[i].rhw = input[i].w;
        vertices[i].color = input[i].color;
        vertices[i].specular = PackedColor(0xff000000);
        vertices[i].t0 = {input[i].u, input[i].v};
    }
    QueueVertices(vertices, count);
    QueueFor(static_cast<TextureMetal*>(mip._texture), mip._level, spec);
    Queue2DPoly(count);
}

void EngineMetal::DrawLine(const Line2DAbs& line, PackedColor color0, PackedColor color1, const Rect2DAbs& clip)
{
    Texture* texture = GPreloadedTextures.New(TextureLine);
    const MipInfo mip = TextBank()->UseMipmap(texture, 1, 1);
    const int spec = NoZBuf | IsAlpha | ClampU | ClampV | IsAlphaFog;
    float x0 = line.beg.x, y0 = line.beg.y;
    float x1 = line.end.x, y1 = line.end.y;
    const float dx = x1 - x0, dy = y1 - y0;
    const float lengthSquared = dx * dx + dy * dy;
    const float inverseLength = lengthSquared > 0.0f ? InvSqrt(lengthSquared) : 1.0f;
    const float px = dy * inverseLength, py = -dx * inverseLength;
    constexpr float width = 3.0f;
    x0 -= px * width * 0.5f;
    x1 -= px * width * 0.5f;
    y0 -= py * width * 0.5f;
    y1 -= py * width * 0.5f;

    Vertex2DAbs vertices[4] = {};
    vertices[0].x = x0;
    vertices[0].y = y0;
    vertices[0].u = 0.0f;
    vertices[0].v = 0.25f;
    vertices[0].color = color0;
    vertices[1].x = x0 + px * width;
    vertices[1].y = y0 + py * width;
    vertices[1].u = 0.0f;
    vertices[1].v = 1.0f;
    vertices[1].color = color0;
    vertices[2].x = x1 + px * width;
    vertices[2].y = y1 + py * width;
    vertices[2].u = 0.1f;
    vertices[2].v = 1.0f;
    vertices[2].color = color1;
    vertices[3].x = x1;
    vertices[3].y = y1;
    vertices[3].u = 0.1f;
    vertices[3].v = 0.25f;
    vertices[3].color = color1;
    DrawPoly(mip, vertices, 4, clip, spec);
}

void EngineMetal::DrawLine(int begin, int end)
{
    if (!_mesh)
        return;
    const TLVertex& first = _mesh->GetVertex(begin);
    const TLVertex& last = _mesh->GetVertex(end);

    float x0 = first.pos.X();
    float y0 = first.pos.Y();
    float x1 = last.pos.X();
    float y1 = last.pos.Y();
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float lengthSquared = dx * dx + dy * dy;
    const float inverseLength = lengthSquared > 0.0f ? InvSqrt(lengthSquared) : 1.0f;
    const float length = lengthSquared * inverseLength;
    const float px = dy * inverseLength;
    const float py = -dx * inverseLength;
    constexpr float width = 3.0f;
    x0 -= px * width * 0.5f;
    x1 -= px * width * 0.5f;
    y0 -= py * width * 0.5f;
    y1 -= py * width * 0.5f;

    Vertex2DAbs vertices[4] = {};
    vertices[0].x = x0;
    vertices[0].y = y0;
    vertices[0].z = first.pos.Z();
    vertices[0].w = first.rhw;
    vertices[0].u = 0.0f;
    vertices[0].v = 0.25f;
    vertices[0].color = first.color;
    vertices[1].x = x0 + px * width;
    vertices[1].y = y0 + py * width;
    vertices[1].z = first.pos.Z();
    vertices[1].w = first.rhw;
    vertices[1].u = 0.0f;
    vertices[1].v = 1.0f;
    vertices[1].color = first.color;
    vertices[2].x = x1 + px * width;
    vertices[2].y = y1 + py * width;
    vertices[2].z = last.pos.Z();
    vertices[2].w = last.rhw;
    vertices[2].u = length;
    vertices[2].v = 1.0f;
    vertices[2].color = last.color;
    vertices[3].x = x1;
    vertices[3].y = y1;
    vertices[3].z = last.pos.Z();
    vertices[3].w = last.rhw;
    vertices[3].u = length;
    vertices[3].v = 0.25f;
    vertices[3].color = last.color;

    Texture* texture = GPreloadedTextures.New(TextureLine);
    const MipInfo mip = TextBank()->UseMipmap(texture, 1, 1);
    const int spec = NoZWrite | IsAlpha | ClampU | ClampV | IsAlphaFog;
    DrawPoly(mip, vertices, 4, Rect2DAbs(0, 0, _w, _h), spec);
}
} // namespace Poseidon
