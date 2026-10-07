#pragma once
#include <functional>
#include <msctf.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace rime_ime::tsf {
// COM tear-off: this interface has the service's identity and lifetime, while
// its Show method is distinct from ITfLangBarItem::Show.
class CandidateUi final : public ITfCandidateListUIElementBehavior {
    IUnknown *owner_;
    std::function<HRESULT(ITfDocumentMgr **)> document_;
    std::function<HRESULT(UINT, bool)> selection_;
    std::function<HRESULT()> abort_;
    std::function<void(bool)> visibility_;
    std::vector<std::wstring> strings_;
    std::vector<UINT> pages_;
    UINT selected_ = 0;
    bool allowed_ = false, shown_ = false;

  public:
    CandidateUi(IUnknown *owner, std::function<HRESULT(ITfDocumentMgr **)> document,
                std::function<HRESULT(UINT, bool)> selection, std::function<HRESULT()> abort,
                std::function<void(bool)> visibility);
    void update(const nlohmann::json &state);
    void clear();
    bool allowed() const {
        return allowed_;
    }
    void permission(bool allowed) {
        allowed_ = allowed;
    }
    void visible(bool shown) {
        shown_ = shown;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **result) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR *description) override;
    HRESULT STDMETHODCALLTYPE GetGUID(GUID *guid) override;
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override;
    HRESULT STDMETHODCALLTYPE IsShown(BOOL *show) override;
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD *flags) override;
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr **document) override;
    HRESULT STDMETHODCALLTYPE GetCount(UINT *count) override;
    HRESULT STDMETHODCALLTYPE GetSelection(UINT *index) override;
    HRESULT STDMETHODCALLTYPE GetString(UINT index, BSTR *text) override;
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT *indices, UINT size, UINT *count) override;
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT *indices, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT *page) override;
    HRESULT STDMETHODCALLTYPE SetSelection(UINT index) override;
    HRESULT STDMETHODCALLTYPE Finalize() override;
    HRESULT STDMETHODCALLTYPE Abort() override;
};
} // namespace rime_ime::tsf
