//
// Copyright 2020 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/pxr.h"
#include "pxr/imaging/glf/glew.h"

#include "pxr/imaging/hdSt/textureBinder.h"
#include "pxr/imaging/hdSt/textureHandle.h"
#include "pxr/imaging/hdSt/textureObject.h"
#include "pxr/imaging/hdSt/samplerObject.h"
#include "pxr/imaging/hdSt/resourceBinder.h"
#include "pxr/imaging/hd/vtBufferSource.h"
#include "pxr/imaging/hgiGL/texture.h"

#include "pxr/imaging/glf/contextCaps.h"

PXR_NAMESPACE_OPEN_SCOPE

bool
HdSt_TextureBinder::UsesBindlessTextures()
{
    return GlfContextCaps::GetInstance().bindlessTextureEnabled;
}

static const HdTupleType _bindlessHandleTupleType{ HdTypeUInt32Vec2, 1 };

void
HdSt_TextureBinder::GetBufferSpecs(
    const NamedTextureHandleVector &textures,
    HdBufferSpecVector * const specs)
{
    for (const NamedTextureHandle & texture : textures) {
        switch (texture.type) {
        case HdTextureType::Uv:
            if (UsesBindlessTextures()) {
                specs->emplace_back(
                    texture.name,
                    _bindlessHandleTupleType);
            }
            break;
        case HdTextureType::Field:
            if (UsesBindlessTextures()) {
                specs->emplace_back(
                    texture.name,
                    _bindlessHandleTupleType);
            }
            specs->emplace_back(
                TfToken(texture.name.GetString() + "SamplingTransform"),
                HdTupleType{HdTypeDoubleMat4, 1});
            break;
        case HdTextureType::Ptex:
            if (UsesBindlessTextures()) {
                specs->emplace_back(
                    texture.name,
                    _bindlessHandleTupleType);
                specs->emplace_back(
                    TfToken(texture.name.GetString() + "_layout"),
                    _bindlessHandleTupleType);
            }
            break;
        default:
            TF_CODING_ERROR("Unsupported texture type");
        }
    }
}

namespace {

// A bindless GL sampler buffer.
// This identifies a texture as a 64-bit handle, passed to GLSL as "uvec2".
// See https://www.khronos.org/opengl/wiki/Bindless_Texture
class HdSt_BindlessSamplerBufferSource : public HdBufferSource {
public:
    HdSt_BindlessSamplerBufferSource(TfToken const &name,
                                     const GLuint64EXT value)
    : HdBufferSource()
    , _name(name)
    , _value(value)
    {
        if (_value == 0) {
            TF_CODING_ERROR("Invalid texture handle: %s: %ld\n",
                            name.GetText(), value);
        }
    }

    ~HdSt_BindlessSamplerBufferSource() override = default;

    TfToken const &GetName() const override {
        return _name;
    }
    void const* GetData() const override {
        return &_value;
    }
    HdTupleType GetTupleType() const override {
        return _bindlessHandleTupleType;
    }
    size_t GetNumElements() const override {
        return 1;
    }
    void GetBufferSpecs(HdBufferSpecVector *specs) const override {
        specs->emplace_back(_name, GetTupleType());
    }
    bool Resolve() override {
        if (!_TryLock()) return false;
        _SetResolved();
        return true;
    }

protected:
    bool _CheckValid() const override {
        return true;
    }

private:
    const TfToken _name;
    const GLuint64EXT _value;
};

class _ComputeBufferSourcesFunctor {
public:
    static void Compute(
        TfToken const &name,
        HdStUvTextureObject const &texture,
        HdStUvSamplerObject const &sampler,
        HdBufferSourceSharedPtrVector * const sources)
    {
        if (!HdSt_TextureBinder::UsesBindlessTextures()) {
            return;
        }

        sources->push_back(
            std::make_shared<HdSt_BindlessSamplerBufferSource>(
                name,
                sampler.GetGLTextureSamplerHandle()));
    }

    static void Compute(
        TfToken const &name,
        HdStFieldTextureObject const &texture,
        HdStFieldSamplerObject const &sampler,
        HdBufferSourceSharedPtrVector * const sources)
    {
        sources->push_back(
            std::make_shared<HdVtBufferSource>(
                TfToken(name.GetString() + "SamplingTransform"),
                VtValue(texture.GetSamplingTransform())));

        if (!HdSt_TextureBinder::UsesBindlessTextures()) {
            return;
        }

        sources->push_back(
            std::make_shared<HdSt_BindlessSamplerBufferSource>(
                name,
                sampler.GetGLTextureSamplerHandle()));
    }

    static void Compute(
        TfToken const &name,
        HdStPtexTextureObject const &texture,
        HdStPtexSamplerObject const &sampler,
        HdBufferSourceSharedPtrVector * const sources)
    {
        if (!HdSt_TextureBinder::UsesBindlessTextures()) {
            return;
        }

        sources->push_back(
            std::make_shared<HdSt_BindlessSamplerBufferSource>(
                name,
                sampler.GetTexelsGLTextureHandle()));

        sources->push_back(
            std::make_shared<HdSt_BindlessSamplerBufferSource>(
                TfToken(name.GetString() + "_layout"),
                sampler.GetLayoutGLTextureHandle()));
    }
};

void
_BindTexture(const GLenum target,
             HgiTextureHandle const &textureHandle,
             GLuint glSamplerName,
             const TfToken &name,
             HdSt_ResourceBinder const &binder,
             const bool bind)
{
    const HdBinding binding = binder.GetBinding(name);
    const int samplerUnit = binding.GetTextureUnit();

    glActiveTexture(GL_TEXTURE0 + samplerUnit);

    const HgiGLTexture * const glTex =
        bind ? dynamic_cast<HgiGLTexture*>(textureHandle.Get()) : nullptr;

    if (glTex) {
        glBindTexture(target, glTex->GetTextureId());
    } else {
        glBindTexture(target, 0);
    }

    glBindSampler(samplerUnit, bind ? glSamplerName : 0);
}

class _BindFunctor {
public:
    static void Compute(
        TfToken const &name,
        HdStUvTextureObject const &texture,
        HdStUvSamplerObject const &sampler,
        HdSt_ResourceBinder const &binder,
        const bool bind)
    {
        _BindTexture(
            GL_TEXTURE_2D,
            texture.GetTexture(),
            sampler.GetGLSamplerName(),
            name,
            binder,
            bind);
    }

    static void Compute(
        TfToken const &name,
        HdStFieldTextureObject const &texture,
        HdStFieldSamplerObject const &sampler,
        HdSt_ResourceBinder const &binder,
        const bool bind)
    {
        _BindTexture(
            GL_TEXTURE_3D,
            texture.GetTexture(),
            sampler.GetGLSamplerName(),
            name,
            binder,
            bind);
    }
    
    static void Compute(
        TfToken const &name,
        HdStPtexTextureObject const &texture,
        HdStPtexSamplerObject const &sampler,
        HdSt_ResourceBinder const &binder,
        const bool bind)
    {
        const HdBinding texelBinding = binder.GetBinding(name);
        const int texelSamplerUnit = texelBinding.GetTextureUnit();

        glActiveTexture(GL_TEXTURE0 + texelSamplerUnit);
        glBindTexture(GL_TEXTURE_2D_ARRAY,
                      bind ? texture.GetTexelGLTextureName() : 0);

        const HdBinding layoutBinding = binder.GetBinding(
            TfToken(name.GetString() + "_layout"));
        const int layoutSamplerUnit = layoutBinding.GetTextureUnit();

        glActiveTexture(GL_TEXTURE0 + layoutSamplerUnit);
        glBindTexture(GL_TEXTURE_BUFFER,
                      bind ? texture.GetLayoutGLTextureName() : 0);
    }
};

template<HdTextureType textureType, class Functor, typename ...Args>
void _CastAndCompute(
    HdStShaderCode::NamedTextureHandle const &namedTextureHandle,
    Args&& ...args)
{
    // e.g. HdStUvTextureObject
    using TextureObject = HdStTypedTextureObject<textureType>;
    // e.g. HdStUvSamplerObject
    using SamplerObject = HdStTypedSamplerObject<textureType>;

    const TextureObject * const typedTexture =
        dynamic_cast<TextureObject *>(
            namedTextureHandle.handle->GetTextureObject().get());
    if (!typedTexture) {
        TF_CODING_ERROR("Bad texture object");
        return;
    }

    const SamplerObject * const typedSampler =
        dynamic_cast<SamplerObject *>(
            namedTextureHandle.handle->GetSamplerObject().get());
    if (!typedSampler) {
        TF_CODING_ERROR("Bad sampler object");
        return;
    }

    Functor::Compute(namedTextureHandle.name, *typedTexture, *typedSampler,
                     std::forward<Args>(args)...);
}

template<class Functor, typename ...Args>
void _Dispatch(
    HdStShaderCode::NamedTextureHandle const &namedTextureHandle,
    Args&& ...args)
{
    switch (namedTextureHandle.type) {
    case HdTextureType::Uv:
        _CastAndCompute<HdTextureType::Uv, Functor>(
            namedTextureHandle, std::forward<Args>(args)...);
        break;
    case HdTextureType::Field:
        _CastAndCompute<HdTextureType::Field, Functor>(
            namedTextureHandle, std::forward<Args>(args)...);
        break;
    case HdTextureType::Ptex:
        _CastAndCompute<HdTextureType::Ptex, Functor>(
            namedTextureHandle, std::forward<Args>(args)...);
        break;
    default:
        TF_CODING_ERROR("Unsupported texture type");
    }
}

template<class Functor, typename ...Args>
void _Dispatch(
    HdStShaderCode::NamedTextureHandleVector const &textures,
    Args &&... args)
{
    for (const HdStShaderCode::NamedTextureHandle & texture : textures) {
        _Dispatch<Functor>(texture, std::forward<Args>(args)...);
    }
}

} // end anonymous namespace

void
HdSt_TextureBinder::ComputeBufferSources(
    const NamedTextureHandleVector &textures,
    HdBufferSourceSharedPtrVector * const sources)
{
    _Dispatch<_ComputeBufferSourcesFunctor>(textures, sources);
}

void
HdSt_TextureBinder::BindResources(
    HdSt_ResourceBinder const &binder,
    const NamedTextureHandleVector &textures)
{
    if (UsesBindlessTextures()) {
        return;
    }

    _Dispatch<_BindFunctor>(textures, binder, /* bind = */ true);
}

void
HdSt_TextureBinder::UnbindResources(
    HdSt_ResourceBinder const &binder,
    const NamedTextureHandleVector &textures)
{
    if (UsesBindlessTextures()) {
        return;
    }

    _Dispatch<_BindFunctor>(textures, binder, /* bind = */ false);
}

PXR_NAMESPACE_CLOSE_SCOPE
