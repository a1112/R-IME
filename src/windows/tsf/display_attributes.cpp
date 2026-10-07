#include "display_attributes.hpp"
#include "guids.hpp"
#include <atomic>
#include <mutex>
#include <new>

namespace rime_ime::tsf {
extern std::atomic<long> objects;
namespace {
TF_DISPLAYATTRIBUTE default_attribute() {
    TF_DISPLAYATTRIBUTE value{};
    // Inherit the application's text/background colors, including dark
    // editors and high contrast. Windows supplies the ordinary input underline.
    value.crText.type = TF_CT_NONE;
    value.crBk.type = TF_CT_NONE;
    value.crLine.type = TF_CT_NONE;
    value.lsStyle = TF_LS_SOLID;
    value.fBoldLine = FALSE;
    value.bAttr = TF_ATTR_INPUT;
    return value;
}
std::mutex attributeMutex;
TF_DISPLAYATTRIBUTE currentAttribute = default_attribute();
class Attribute final : public ITfDisplayAttributeInfo {
    ULONG refs_ = 1;

  public:
    Attribute() {
        ++objects;
    }
    ~Attribute() {
        --objects;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **result) override {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (id == IID_IUnknown || id == IID_ITfDisplayAttributeInfo)
            *result = static_cast<ITfDisplayAttributeInfo *>(this);
        if (!*result)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto count = --refs_;
        if (!count)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE GetGUID(GUID *guid) override {
        if (!guid)
            return E_POINTER;
        *guid = input_attribute_id;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR *description) override {
        if (!description)
            return E_POINTER;
        *description = SysAllocString(L"R-IME 未转换输入");
        return *description ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetAttributeInfo(TF_DISPLAYATTRIBUTE *value) override {
        if (!value)
            return E_POINTER;
        std::lock_guard lock(attributeMutex);
        *value = currentAttribute;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetAttributeInfo(const TF_DISPLAYATTRIBUTE *value) override {
        if (!value)
            return E_POINTER;
        if (value->lsStyle < TF_LS_NONE || value->lsStyle > TF_LS_SQUIGGLE ||
            value->bAttr < TF_ATTR_OTHER || value->bAttr > TF_ATTR_FIXEDCONVERTED)
            return E_INVALIDARG;
        for (const auto &color : {value->crText, value->crBk, value->crLine})
            if (color.type != TF_CT_NONE && color.type != TF_CT_SYSCOLOR &&
                color.type != TF_CT_COLORREF)
                return E_INVALIDARG;
        std::lock_guard lock(attributeMutex);
        currentAttribute = *value;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        std::lock_guard lock(attributeMutex);
        currentAttribute = default_attribute();
        return S_OK;
    }
};
class AttributeEnum final : public IEnumTfDisplayAttributeInfo {
    ULONG refs_ = 1, position_ = 0;

  public:
    explicit AttributeEnum(ULONG position = 0) : position_(position) {
        ++objects;
    }
    ~AttributeEnum() {
        --objects;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **result) override {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (id == IID_IUnknown || id == IID_IEnumTfDisplayAttributeInfo)
            *result = static_cast<IEnumTfDisplayAttributeInfo *>(this);
        if (!*result)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto count = --refs_;
        if (!count)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Clone(IEnumTfDisplayAttributeInfo **copy) override {
        if (!copy)
            return E_POINTER;
        *copy = new (std::nothrow) AttributeEnum(position_);
        return *copy ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, ITfDisplayAttributeInfo **values,
                                   ULONG *fetched) override {
        if ((count && !values) || (!fetched && count != 1))
            return E_POINTER;
        if (fetched)
            *fetched = 0;
        if (!count)
            return S_OK;
        values[0] = nullptr;
        if (position_)
            return S_FALSE;
        auto value = new (std::nothrow) Attribute;
        if (!value)
            return E_OUTOFMEMORY;
        values[0] = value;
        position_ = 1;
        if (fetched)
            *fetched = 1;
        return count == 1 ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        position_ = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        auto remaining = position_ ? 0u : 1u;
        if (count)
            position_ = 1;
        return count <= remaining ? S_OK : S_FALSE;
    }
};
} // namespace
HRESULT input_attribute(REFGUID guid, ITfDisplayAttributeInfo **info) {
    if (!info)
        return E_POINTER;
    *info = nullptr;
    if (guid != input_attribute_id)
        return E_INVALIDARG;
    *info = new (std::nothrow) Attribute;
    return *info ? S_OK : E_OUTOFMEMORY;
}
HRESULT enumerate_attributes(IEnumTfDisplayAttributeInfo **attributes) {
    if (!attributes)
        return E_POINTER;
    *attributes = new (std::nothrow) AttributeEnum;
    return *attributes ? S_OK : E_OUTOFMEMORY;
}
} // namespace rime_ime::tsf
