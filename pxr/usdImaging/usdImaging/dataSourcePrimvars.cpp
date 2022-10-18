//
// Copyright 2022 Pixar
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

#include "pxr/usdImaging/usdImaging/dataSourcePrimvars.h"
#include "pxr/usdImaging/usdImaging/dataSourceRelationship.h"

#include "pxr/usdImaging/usdImaging/primvarUtils.h"

#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"

PXR_NAMESPACE_OPEN_SCOPE

UsdImagingDataSourcePrimvars::UsdImagingDataSourcePrimvars(
    const SdfPath &sceneIndexPath,
    UsdPrim const &usdPrim,
    UsdGeomPrimvarsAPI usdPrimvars,
    const CustomPrimvarMapping &customPrimvarMapping,
    const UsdImagingDataSourceStageGlobals & stageGlobals)
: _sceneIndexPath(sceneIndexPath)
, _usdPrim(usdPrim)
, _stageGlobals(stageGlobals)
{
    std::vector<UsdGeomPrimvar> primvars = usdPrimvars.GetPrimvars();
    for (const auto & p : primvars) {
        _namespacedPrimvars[p.GetPrimvarName()] = p;
    }

    for (const auto & cp : customPrimvarMapping) {
        UsdAttributeQuery aQ(usdPrimvars.GetPrim().GetAttribute(cp.second));
        if (aQ.HasAuthoredValue()) {
            _customPrimvars[cp.first] = aQ;
        }
    }
}


/*static*/
TfToken
UsdImagingDataSourcePrimvars::_GetPrefixedName(const TfToken &name)
{
    return TfToken(("primvars:" + name.GetString()).c_str());
}

bool
UsdImagingDataSourcePrimvars::Has(const TfToken & name)
{
    TRACE_FUNCTION();

    _NamespacedPrimvarsMap::const_iterator nsIt = _namespacedPrimvars.find(name);
    if (nsIt != _namespacedPrimvars.end()) {
        return true;
    }

    _CustomPrimvarsMap::const_iterator cIt = _customPrimvars.find(name);
    if (cIt != _customPrimvars.end()) {
        return true;
    }


    if (UsdRelationship rel =
            _usdPrim.GetRelationship(_GetPrefixedName(name))) {
        return true;
    }


    return false;
}

TfTokenVector 
UsdImagingDataSourcePrimvars::GetNames()
{
    TRACE_FUNCTION();

    TfTokenVector result;
    result.reserve(_customPrimvars.size() + _namespacedPrimvars.size());

    for (const auto & entry : _namespacedPrimvars) {
        result.push_back(entry.first);
    }

    for (const auto & entry : _customPrimvars) {
        result.push_back(entry.first);
    }

    for (UsdProperty prop :
            _usdPrim.GetAuthoredPropertiesInNamespace("primvars:")) {
        if (UsdRelationship rel = prop.As<UsdRelationship>()) {
            // strip only the "primvars:" namespace
            static const size_t prefixLength = 9;
            result.push_back(TfToken(rel.GetName().data() + prefixLength));
        }
    }

    return result;
}

HdDataSourceBaseHandle
UsdImagingDataSourcePrimvars::Get(const TfToken & name)
{
    TRACE_FUNCTION();

    _NamespacedPrimvarsMap::const_iterator nsIt =
        _namespacedPrimvars.find(name);
    if (nsIt != _namespacedPrimvars.end()) {
        return UsdImagingDataSourcePrimvar::New(
                _sceneIndexPath, name, _stageGlobals,
                UsdAttributeQuery(nsIt->second.GetAttr()) /* value */,
                UsdAttributeQuery(nsIt->second.GetIndicesAttr()) /* indices */,
                HdPrimvarSchema::BuildInterpolationDataSource(
                    UsdImagingUsdToHdInterpolationToken(
                        nsIt->second.GetInterpolation())),
                HdPrimvarSchema::BuildRoleDataSource(
                    UsdImagingUsdToHdRole(nsIt->second.GetAttr().GetRoleName())));
    }

    _CustomPrimvarsMap::const_iterator cIt = _customPrimvars.find(name);
    if (cIt != _customPrimvars.end()) {
        return UsdImagingDataSourcePrimvar::New(
            _sceneIndexPath, name, _stageGlobals,
            cIt->second /* value */, UsdAttributeQuery() /* indices */,
            HdPrimvarSchema::BuildInterpolationDataSource(
                UsdImagingUsdToHdInterpolationToken(
                    _GetCustomPrimvarInterpolation(cIt->second))),
            HdPrimvarSchema::BuildRoleDataSource(
                UsdImagingUsdToHdRole(cIt->second.GetAttribute().GetRoleName())));
    }


    if (UsdRelationship rel =
            _usdPrim.GetRelationship(_GetPrefixedName(name))) {

        return HdPrimvarSchema::Builder()
            .SetPrimvarValue(UsdImagingDataSourceRelationship::New(
                rel, _stageGlobals))
            .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                HdPrimvarSchemaTokens->constant))
            .Build();
    }

    return nullptr;
}

TfToken
UsdImagingDataSourcePrimvars::_GetCustomPrimvarInterpolation(
        const UsdAttributeQuery &attrQuery)
{
    // A reimplementation of UsdGeomPrimvar::GetInterpolation(),
    // but with "vertex" as the default instead of "constant"...
    TfToken interpolation;
    if (attrQuery.GetAttribute().GetMetadata(
            UsdGeomTokens->interpolation, &interpolation)) {
        return interpolation;
    }

    return UsdGeomTokens->vertex;
}

// ----------------------------------------------------------------------------

static inline bool
_IsIndexed(const UsdAttributeQuery& indicesQuery)
{
    return indicesQuery.IsValid() && indicesQuery.HasValue();
}

UsdImagingDataSourcePrimvar::UsdImagingDataSourcePrimvar(
        const SdfPath &sceneIndexPath,
        const TfToken &name,
        const UsdImagingDataSourceStageGlobals &stageGlobals,
        UsdAttributeQuery valueQuery,
        UsdAttributeQuery indicesQuery,
        HdTokenDataSourceHandle interpolation,
        HdTokenDataSourceHandle role)
: _stageGlobals(stageGlobals)
, _valueQuery(valueQuery)
, _indicesQuery(indicesQuery)
, _interpolation(interpolation)
, _role(role)
{
    const bool indexed = _IsIndexed(_indicesQuery);
    if (indexed) {
        if (_valueQuery.ValueMightBeTimeVarying()) {
            _stageGlobals.FlagAsTimeVarying(sceneIndexPath,
                    HdDataSourceLocator(
                        HdPrimvarsSchemaTokens->primvars,
                        name,
                        HdPrimvarSchemaTokens->indexedPrimvarValue));
        }
        if (_indicesQuery.ValueMightBeTimeVarying()) {
            _stageGlobals.FlagAsTimeVarying(sceneIndexPath,
                    HdDataSourceLocator(
                        HdPrimvarsSchemaTokens->primvars,
                        name,
                        HdPrimvarSchemaTokens->indices));
        }
    } else {
        if (_valueQuery.ValueMightBeTimeVarying()) {
            _stageGlobals.FlagAsTimeVarying(sceneIndexPath,
                    HdDataSourceLocator(
                        HdPrimvarsSchemaTokens->primvars,
                        name,
                        HdPrimvarSchemaTokens->primvarValue));
        }
    }
}

bool
UsdImagingDataSourcePrimvar::Has(const TfToken & name)
{
    const bool indexed = _IsIndexed(_indicesQuery);

    if (indexed) {
        return
            name == HdPrimvarSchemaTokens->indexedPrimvarValue ||
            name == HdPrimvarSchemaTokens->indices ||
            name == HdPrimvarSchemaTokens->interpolation ||
            name == HdPrimvarSchemaTokens->role;
    } else {
        return
            name == HdPrimvarSchemaTokens->primvarValue ||
            name == HdPrimvarSchemaTokens->interpolation ||
            name == HdPrimvarSchemaTokens->role;
    }
}

TfTokenVector
UsdImagingDataSourcePrimvar::GetNames()
{
    const bool indexed = _IsIndexed(_indicesQuery);

    TfTokenVector result = {
        HdPrimvarSchemaTokens->interpolation,
        HdPrimvarSchemaTokens->role,
    };
    
    if (indexed) {
        result.push_back(HdPrimvarSchemaTokens->indexedPrimvarValue);
        result.push_back(HdPrimvarSchemaTokens->indices);
    } else {
        result.push_back(HdPrimvarSchemaTokens->primvarValue);
    }

    return result;
}

HdDataSourceBaseHandle
UsdImagingDataSourcePrimvar::Get(const TfToken & name)
{
    TRACE_FUNCTION();

    const bool indexed = _IsIndexed(_indicesQuery);

    if (indexed) {
        if (name == HdPrimvarSchemaTokens->indexedPrimvarValue) {
            return UsdImagingDataSourceAttributeNew(
                    _valueQuery, _stageGlobals);
        } else if (name == HdPrimvarSchemaTokens->indices) {
            return UsdImagingDataSourceAttributeNew(
                    _indicesQuery, _stageGlobals);
        }
    } else {
        if (name == HdPrimvarSchemaTokens->primvarValue) {
            return UsdImagingDataSourceAttributeNew(
                    _valueQuery, _stageGlobals);
        }
    }

    if (name == HdPrimvarSchemaTokens->interpolation) {
        return _interpolation;
    } else if (name == HdPrimvarSchemaTokens->role) {
        return _role;
    }
    return nullptr;
}

PXR_NAMESPACE_CLOSE_SCOPE
