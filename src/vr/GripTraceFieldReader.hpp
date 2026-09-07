#pragma once

namespace gakumas::vr {
// ECMA/IL2CPP FieldAttributes.Static. UnityResolve's cached static_field uses
// offset <= 0, which misclassifies static fields at positive static-data offsets.
constexpr int GripFieldStatic = 0x10;
template<class Flags, class ReadValue>
bool ReadGripInstanceReference(void* object, void* field, Flags flags,
                               ReadValue readValue, void*& result) {
    result = nullptr;
    if (!object || !field || (flags(field) & GripFieldStatic)) return false;
    readValue(object, field, &result);
    return true;
}
} // namespace gakumas::vr
