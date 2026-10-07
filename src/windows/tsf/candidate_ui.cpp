#include "candidate_ui.hpp"
#include "guids.hpp"
#include "panel.hpp"
#include <algorithm>

namespace rime_ime::tsf {
CandidateUi::CandidateUi(IUnknown *owner, std::function<HRESULT(ITfDocumentMgr **)> document,
                         std::function<HRESULT(UINT, bool)> selection,
                         std::function<HRESULT()> abort, std::function<void(bool)> visibility)
    : owner_(owner), document_(std::move(document)), selection_(std::move(selection)),
      abort_(std::move(abort)), visibility_(std::move(visibility)) {}
void CandidateUi::update(const nlohmann::json &state) {
    std::vector<std::wstring> next;
    for (const auto &item : state.at("candidates"))
        next.push_back(wide(item.value("text", "")));
    // The Rime protocol supplies the current engine page. Expose those real
    // strings as one available batch, never invent strings on unqueried pages.
    if (next != strings_)
        pages_ = next.empty() ? std::vector<UINT>{} : std::vector<UINT>{0};
    strings_ = std::move(next);
    selected_ = static_cast<UINT>(
        std::clamp(state.value("highlighted", 0), 0, std::max(0, int(strings_.size()) - 1)));
}
void CandidateUi::clear() {
    strings_.clear();
    pages_.clear();
    selected_ = 0;
    allowed_ = shown_ = false;
}
HRESULT STDMETHODCALLTYPE CandidateUi::QueryInterface(REFIID id, void **result) {
    return owner_->QueryInterface(id, result);
}
ULONG STDMETHODCALLTYPE CandidateUi::AddRef() {
    return owner_->AddRef();
}
ULONG STDMETHODCALLTYPE CandidateUi::Release() {
    return owner_->Release();
}
HRESULT STDMETHODCALLTYPE CandidateUi::GetDescription(BSTR *description) {
    if (!description)
        return E_POINTER;
    *description = SysAllocString(L"R-IME 候选列表");
    return *description ? S_OK : E_OUTOFMEMORY;
}
HRESULT STDMETHODCALLTYPE CandidateUi::GetGUID(GUID *guid) {
    if (!guid)
        return E_POINTER;
    *guid = candidate_ui_id;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateUi::Show(BOOL show) {
    allowed_ = show != FALSE;
    visibility_(allowed_);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateUi::IsShown(BOOL *show) {
    if (!show)
        return E_POINTER;
    *show = shown_;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateUi::GetUpdatedFlags(DWORD *flags) {
    if (!flags)
        return E_POINTER;
    *flags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING |
             TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateUi::GetDocumentMgr(ITfDocumentMgr **document) {
    if (!document)
        return E_POINTER;
    *document = nullptr;
    return document_(document);
}
HRESULT STDMETHODCALLTYPE CandidateUi::GetCount(UINT *count) {
    if (!count)
        return E_POINTER;
    *count = static_cast<UINT>(strings_.size());
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateUi::GetSelection(UINT *index) {
    if (!index)
        return E_POINTER;
    *index = selected_;
    return strings_.empty() ? S_FALSE : S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateUi::GetString(UINT index, BSTR *text) {
    if (!text)
        return E_POINTER;
    *text = nullptr;
    if (index >= strings_.size())
        return E_INVALIDARG;
    *text = SysAllocStringLen(strings_[index].data(), static_cast<UINT>(strings_[index].size()));
    return *text ? S_OK : E_OUTOFMEMORY;
}
HRESULT STDMETHODCALLTYPE CandidateUi::GetPageIndex(UINT *indices, UINT size, UINT *count) {
    if (!count || (size && !indices))
        return E_POINTER;
    *count = static_cast<UINT>(pages_.size());
    auto copied = std::min<size_t>(size, pages_.size());
    if (copied)
        std::copy_n(pages_.begin(), copied, indices);
    return size < pages_.size() ? S_FALSE : S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateUi::SetPageIndex(UINT *indices, UINT count) {
    if (!indices || !count || strings_.empty() || indices[0] != 0)
        return E_INVALIDARG;
    for (UINT i = 0; i < count; ++i)
        if (indices[i] >= strings_.size() || (i && indices[i] <= indices[i - 1]))
            return E_INVALIDARG;
    pages_.assign(indices, indices + count);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateUi::GetCurrentPage(UINT *page) {
    if (!page)
        return E_POINTER;
    *page = 0;
    if (pages_.empty())
        return S_FALSE;
    *page = static_cast<UINT>(std::upper_bound(pages_.begin(), pages_.end(), selected_) -
                              pages_.begin() - 1);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE CandidateUi::SetSelection(UINT index) {
    if (index >= strings_.size())
        return E_INVALIDARG;
    return selection_(index, false);
}
HRESULT STDMETHODCALLTYPE CandidateUi::Finalize() {
    if (strings_.empty())
        return E_UNEXPECTED;
    return selection_(selected_, true);
}
HRESULT STDMETHODCALLTYPE CandidateUi::Abort() {
    return strings_.empty() ? S_FALSE : abort_();
}
} // namespace rime_ime::tsf
