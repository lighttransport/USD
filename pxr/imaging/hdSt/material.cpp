//
// Copyright 2016 Pixar
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
#include "pxr/imaging/glf/glew.h"

#include "pxr/imaging/hdSt/material.h"
#include "pxr/imaging/hdSt/materialBufferSourceAndTextureHelper.h"
#include "pxr/imaging/hdSt/debugCodes.h"
#include "pxr/imaging/hdSt/package.h"
#include "pxr/imaging/hdSt/resourceRegistry.h"
#include "pxr/imaging/hdSt/shaderCode.h"
#include "pxr/imaging/hdSt/surfaceShader.h"
#include "pxr/imaging/hdSt/textureBinder.h"
#include "pxr/imaging/hdSt/textureHandle.h"
#include "pxr/imaging/hdSt/textureResource.h"
#include "pxr/imaging/hdSt/textureResourceHandle.h"
#include "pxr/imaging/hdSt/tokens.h"

#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/imaging/glf/contextCaps.h"
#include "pxr/imaging/glf/textureHandle.h"
#include "pxr/imaging/glf/textureRegistry.h"
#include "pxr/imaging/glf/uvTextureStorage.h"
#include "pxr/imaging/hio/glslfx.h"

#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tf/staticTokens.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_ENV_SETTING(HDST_USE_NEW_TEXTURE_SYSTEM, false,
                      "Use new texture system for Storm.");

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (limitSurfaceEvaluation)
    (opacity)
);

HioGlslfx *HdStMaterial::_fallbackGlslfx = nullptr;


HdStMaterial::HdStMaterial(SdfPath const &id)
 : HdMaterial(id)
 , _surfaceShader(new HdStSurfaceShader)
 , _isInitialized(false)
 , _hasPtex(false)
 , _hasLimitSurfaceEvaluation(false)
 , _hasDisplacement(false)
 , _materialTag(HdStMaterialTagTokens->defaultMaterialTag)
{
    TF_DEBUG(HDST_MATERIAL_ADDED).Msg("HdStMaterial Created: %s\n",
                                      id.GetText());
}

HdStMaterial::~HdStMaterial()
{
    TF_DEBUG(HDST_MATERIAL_REMOVED).Msg("HdStMaterial Removed: %s\n",
                                        GetId().GetText());
}

// The new texture system does not support all HdTextureType's yet.
// Use old texture system for those.
static
bool
_IsSupportedByNewTextureSystem(const HdTextureType type)
{
    switch(type) {
    case HdTextureType::Uv:
    case HdTextureType::Field:
    case HdTextureType::Ptex:
        return true;
    default:
        return false;
    }
}

// Use data authored on material network nodes to create
// textures with the new texture system.
static
HdStShaderCode::NamedTextureHandleVector
_GetNamedTextureHandles(
    HdStMaterialNetwork::TextureDescriptorVector const &descs,
    std::weak_ptr<HdStShaderCode> const &shaderCode,
    HdStResourceRegistrySharedPtr const& resourceRegistry)
{
    const bool usesBindlessTextures =
        HdSt_TextureBinder::UsesBindlessTextures();

    HdStShaderCode::NamedTextureHandleVector result;

    for (HdStMaterialNetwork::TextureDescriptor const &desc : descs) {

        if (_IsSupportedByNewTextureSystem(desc.type)) {

            HdStTextureHandleSharedPtr const textureHandle =
                resourceRegistry->AllocateTextureHandle(
                    desc.textureId,
                    desc.type,
                    desc.samplerParameters,
                    desc.memoryRequest,
                    usesBindlessTextures,
                    shaderCode);

            result.push_back({ desc.name,
                               desc.type,
                               textureHandle });
        }
    }

    return result;
}

/* virtual */
void
HdStMaterial::Sync(HdSceneDelegate *sceneDelegate,
                 HdRenderParam   *renderParam,
                 HdDirtyBits     *dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    TF_UNUSED(renderParam);

    HdStResourceRegistrySharedPtr const& resourceRegistry =
        std::static_pointer_cast<HdStResourceRegistry>(
            sceneDelegate->GetRenderIndex().GetResourceRegistry());

    HdDirtyBits bits = *dirtyBits;

    if (!(bits & DirtyResource) && !(bits & DirtyParams)) {
        *dirtyBits = Clean;
        return;
    }

    const bool useNewTextureSystem =
        TfGetEnvSetting(HDST_USE_NEW_TEXTURE_SYSTEM);

    bool needsRprimMaterialStateUpdate = false;

    std::string fragmentSource;
    std::string geometrySource;
    VtDictionary materialMetadata;
    TfToken materialTag = _materialTag;
    HdSt_MaterialParamVector params;
    HdStMaterialNetwork::TextureDescriptorVector textureDescriptors;

    VtValue vtMat = sceneDelegate->GetMaterialResource(GetId());
    if (vtMat.IsHolding<HdMaterialNetworkMap>()) {
        HdMaterialNetworkMap const& hdNetworkMap =
            vtMat.UncheckedGet<HdMaterialNetworkMap>();
        if (!hdNetworkMap.terminals.empty() && !hdNetworkMap.map.empty()) {
            _networkProcessor.ProcessMaterialNetwork(GetId(), hdNetworkMap);
            fragmentSource = _networkProcessor.GetFragmentCode();
            geometrySource = _networkProcessor.GetGeometryCode();
            materialMetadata = _networkProcessor.GetMetadata();
            materialTag = _networkProcessor.GetMaterialTag();
            params = _networkProcessor.GetMaterialParams();
            if (useNewTextureSystem) {
                textureDescriptors = _networkProcessor.GetTextureDescriptors();
            }
        }
    }

    if (fragmentSource.empty() && geometrySource.empty()) {
        _InitFallbackShader();
        fragmentSource = _fallbackGlslfx->GetSurfaceSource();
        // Note that we don't want displacement on purpose for the 
        // fallback material.
        geometrySource = std::string();
        materialMetadata = _fallbackGlslfx->GetMetadata();
    }

    _surfaceShader->SetFragmentSource(fragmentSource);
    _surfaceShader->SetGeometrySource(geometrySource);

    bool hasDisplacement = !(geometrySource.empty());

    if (_hasDisplacement != hasDisplacement) {
        _hasDisplacement = hasDisplacement;
        needsRprimMaterialStateUpdate = true;
    }

    bool hasLimitSurfaceEvaluation =
        _GetHasLimitSurfaceEvaluation(materialMetadata);

    if (_hasLimitSurfaceEvaluation != hasLimitSurfaceEvaluation) {
        _hasLimitSurfaceEvaluation = hasLimitSurfaceEvaluation;
        needsRprimMaterialStateUpdate = true;
    }

    if (_materialTag != materialTag) {
        _materialTag = materialTag;
        _surfaceShader->SetMaterialTag(_materialTag);
        needsRprimMaterialStateUpdate = true;
    }

    _surfaceShader->SetEnabledPrimvarFiltering(true);

    //
    // Mark batches dirty to force batch validation/rebuild.
    //

    if (_isInitialized) {

        // We need to re-batch when the shader or materialTag changes. I.e. when
        // network topology changes or the prim goes from opaque to translucent.
        // We skip this the first time since batches will already be rebuild.

        bool markBatchesDirty = (_materialTag != materialTag);

        if (!markBatchesDirty) {
            // XXX cheaper to compare network topology instead fo strings?
            std::string const& oldFragmentSource = 
                _surfaceShader->GetSource(HdShaderTokens->fragmentShader);
            std::string const& oldGeometrySource = 
                _surfaceShader->GetSource(HdShaderTokens->geometryShader);

            markBatchesDirty |= (oldFragmentSource!=fragmentSource) || 
                                (oldGeometrySource!=geometrySource);
        }

        if (markBatchesDirty) {
            sceneDelegate->GetRenderIndex().GetChangeTracker().
                MarkBatchesDirty();
        }
    }

    //
    // Update material parameters
    //
    _surfaceShader->SetParams(params);

    // Release any fallback texture resources
    _internalTextureResourceHandles.clear();

    HdBufferSpecVector specs;
    HdBufferSourceSharedPtrVector sources;

    // Texture descriptors for the old texture system.
    HdStShaderCode::TextureDescriptorVector textureResourceDescriptors;

    bool hasPtex = false;
    for (HdSt_MaterialParam const & param: params) {
        if (param.IsPrimvarRedirect() || param.IsFallback()) {
            HdStSurfaceShader::AddFallbackValueToSpecsAndSources(
                param, &specs, &sources);
        } else if (param.IsTexture()) {
            if (param.textureType == HdTextureType::Ptex) {
                hasPtex = true;
            }

            // Use the old texture system unless the environment
            // variable is set and the texture type can be handled
            // by the new texture system.
            //
            if (!(useNewTextureSystem &&
                  _IsSupportedByNewTextureSystem(param.textureType))) {

                HdSt_MaterialBufferSourceAndTextureHelper::
                    ProcessTextureMaterialParam(
                        param,
                        _GetTextureResourceHandle(sceneDelegate, param),
                        &specs, &sources, &textureResourceDescriptors);
            }
        }
    }

    _surfaceShader->SetTextureDescriptors(textureResourceDescriptors);

    if (useNewTextureSystem) {
        // Create textures for those texture types supported
        // by the new texture system.
        const HdStShaderCode::NamedTextureHandleVector textures =
            _GetNamedTextureHandles(
                textureDescriptors,
                _surfaceShader,
                resourceRegistry);

        _surfaceShader->SetNamedTextureHandles(textures);

        HdSt_TextureBinder::GetBufferSpecs(textures, &specs);
    }

    _surfaceShader->SetBufferSources(specs, sources, resourceRegistry);

    if (_hasPtex != hasPtex) {
        _hasPtex = hasPtex;
        needsRprimMaterialStateUpdate = true;
    }


    if (needsRprimMaterialStateUpdate && _isInitialized) {
        // XXX Forcing rprims to have a dirty material id to re-evaluate
        // their material state as we don't know which rprims are bound to
        // this one. We can skip this invalidation the first time this
        // material is Sync'd since any affected Rprim should already be
        // marked with a dirty material id.
        HdChangeTracker& changeTracker =
                         sceneDelegate->GetRenderIndex().GetChangeTracker();
        changeTracker.MarkAllRprimsDirty(HdChangeTracker::DirtyMaterialId);
    }

    _isInitialized = true;
    *dirtyBits = Clean;
}

HdStTextureResourceHandleSharedPtr
HdStMaterial::_GetTextureResourceHandle(
        HdSceneDelegate *sceneDelegate,
        HdSt_MaterialParam const &param)
{
    HdStResourceRegistrySharedPtr const& resourceRegistry =
        std::static_pointer_cast<HdStResourceRegistry>(
            sceneDelegate->GetRenderIndex().GetResourceRegistry());

    HdStTextureResourceSharedPtr texResource;
    HdStTextureResourceHandleSharedPtr handle;

    SdfPath const &connection = param.connection;
    if (!connection.IsEmpty()) {
        HdTextureResource::ID texID =
            GetTextureResourceID(sceneDelegate, connection);

        // Step 1.
        // Try to locate the texture in resource registry.
        // A Bprim might have been inserted for this texture.
        //
        if (texID != HdTextureResource::ID(-1)) {
            // Use render index to convert local texture id into global
            // texture key
            HdRenderIndex &renderIndex = sceneDelegate->GetRenderIndex();
            HdResourceRegistry::TextureKey texKey =
                                               renderIndex.GetTextureKey(texID);

            bool textureResourceFound = false;
            HdInstance<HdStTextureResourceSharedPtr> texInstance =
                resourceRegistry->FindTextureResource
                                  (texKey, &textureResourceFound);

            // A bad asset can cause the texture resource to not
            // be found. Hence, issue a warning and continue onto the
            // next param.
            if (!textureResourceFound) {
                TF_WARN("No texture resource found with path %s",
                    param.connection.GetText());
            } else {
                texResource = texInstance.GetValue();
            }
        }

        HdResourceRegistry::TextureKey handleKey =
            HdStTextureResourceHandle::GetHandleKey(
                &sceneDelegate->GetRenderIndex(), connection);

        bool handleFound = false;
        HdInstance<HdStTextureResourceHandleSharedPtr> handleInstance =
            resourceRegistry->FindTextureResourceHandle
                              (handleKey, &handleFound);

        if (handleFound) {
            handle = handleInstance.GetValue();
            handle->SetTextureResource(texResource);
        }

        // Step 2.
        // If no texture was found in the registry, it might be a texture we
        // discovered in the material network. If we can load it we will store
        // the handle internally in this material.
        //
        if (!texResource) {
            HdTextureResourceSharedPtr hdTexResource = 
                sceneDelegate->GetTextureResource(connection);
            texResource = std::static_pointer_cast<HdStTextureResource>(
                hdTexResource);
        }
    }

    // There are many reasons why texResource could be null here:
    // - A missing or invalid connection path,
    // - A deliberate (-1) or accidental invalid texture id
    // - Scene delegate failed to return a texture resource (due to asset error)
    //
    // In all these cases fallback to a simple texture with the provided
    // fallback value
    //
    // XXX todo handle fallback Ptex textures
    if (!(handle && handle->GetTextureResource())) {

        if (!texResource) {
            // A bad asset can cause the texture resource to not
            // be found. Hence, issue a warning and insert a fallback texture.
            if (!param.connection.IsEmpty()) {
                TF_WARN("Texture not found. Using fallback texture for: %s",
                        param.connection.GetText());
            }

            // Fallback texture are only supported for UV textures.
            if (param.textureType != HdTextureType::Uv) {
                return {};
            }
            GlfUVTextureStorageRefPtr texPtr =
                GlfUVTextureStorage::New(1,1, param.fallbackValue);
            GlfTextureHandleRefPtr texture =
                GlfTextureRegistry::GetInstance().GetTextureHandle(texPtr);

            texResource = HdStTextureResourceSharedPtr(
                new HdStSimpleTextureResource(texture,
                                              HdTextureType::Uv,
                                              HdWrapClamp,
                                              HdWrapClamp,
                                              HdWrapClamp,
                                              HdMinFilterNearest,
                                              HdMagFilterNearest,
                                              0));
        }

        if (texResource) {
            handle.reset(new HdStTextureResourceHandle(texResource));
            _internalTextureResourceHandles.push_back(handle);
        }
    }

    return handle;
}

bool
HdStMaterial::_GetHasLimitSurfaceEvaluation(VtDictionary const & metadata) const
{
    VtValue value = TfMapLookupByValue(metadata,
                                       _tokens->limitSurfaceEvaluation,
                                       VtValue());
    return value.IsHolding<bool>() && value.Get<bool>();
}

// virtual
HdDirtyBits
HdStMaterial::GetInitialDirtyBitsMask() const
{
    return AllDirty;
}


//virtual
void
HdStMaterial::Reload()
{
    _networkProcessor.ClearGlslfx();
    _surfaceShader->Reload();
}

HdStShaderCodeSharedPtr
HdStMaterial::GetShaderCode() const
{
    return _surfaceShader;
}

void
HdStMaterial::SetSurfaceShader(HdStSurfaceShaderSharedPtr &shaderCode)
{
    _surfaceShader = shaderCode;
}

void
HdStMaterial::_InitFallbackShader()
{
    if (_fallbackGlslfx != nullptr) {
        return;
    }

    const TfToken &filePath = HdStPackageFallbackSurfaceShader();

    _fallbackGlslfx = new HioGlslfx(filePath);

    // Check fallback shader loaded, if not continue with the invalid shader
    // this would mean the shader compilation fails and the prim would not
    // be drawn.
    TF_VERIFY(_fallbackGlslfx->IsValid(),
              "Failed to load fallback surface shader!");
}

PXR_NAMESPACE_CLOSE_SCOPE
