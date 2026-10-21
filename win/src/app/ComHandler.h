#pragma once
// Minimal COM callback object for WebView2 completion/event handlers, so the
// tree does not depend on WRL's Callback<> (and builds with any toolchain).
#include "base/Win.h"

#include <functional>
#include <utility>

namespace vela::app {

template <typename Interface, const IID& InterfaceId, typename... Args>
class Handler final : public Interface {
public:
    using Fn = std::function<HRESULT(Args...)>;
    static Interface* make(Fn fn) { return new Handler(std::move(fn)); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, InterfaceId)) {
            *out = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return ULONG(r);
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return fn_(args...); }

private:
    explicit Handler(Fn fn) : fn_(std::move(fn)) {}
    Fn fn_;
    volatile LONG refs_ = 1;
};

} // namespace vela::app
