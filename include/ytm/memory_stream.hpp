#pragma once
#include <windows.h>
#include <objidl.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace ytm {
// IStream de solo lectura sobre un buffer compartido. No copia los datos: todas las instancias
// (incluidos los clones que cree Media Foundation) comparten el mismo vector inmutable.
class SharedMemoryStream final : public IStream {
public:
    explicit SharedMemoryStream(std::shared_ptr<const std::vector<uint8_t>> data, ULONGLONG position = 0)
        : data_(std::move(data)), position_(position) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) { *out = static_cast<IStream*>(this); AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&refs_); if (!n) delete this; return (ULONG)n; }

    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG count, ULONG* read) override {
        if (read) *read = 0;
        if (!buffer && count) return STG_E_INVALIDPOINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        const ULONGLONG size = data_ ? data_->size() : 0;
        ULONG copied = 0;
        if (position_ < size) {
            const ULONGLONG left = size - position_;
            copied = (ULONG)(left < count ? left : count);
            if (copied) std::memcpy(buffer, data_->data() + (size_t)position_, copied);
            position_ += copied;
        }
        if (read) *read = copied;
        return copied == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPosition) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const LONGLONG size = data_ ? (LONGLONG)data_->size() : 0;
        LONGLONG base = 0;
        if (origin == STREAM_SEEK_SET) base = 0;
        else if (origin == STREAM_SEEK_CUR) base = (LONGLONG)position_;
        else if (origin == STREAM_SEEK_END) base = size;
        else return STG_E_INVALIDFUNCTION;
        const LONGLONG target = base + move.QuadPart;
        if (target < 0) return STG_E_INVALIDFUNCTION;
        position_ = (ULONGLONG)target;
        if (newPosition) newPosition->QuadPart = position_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        std::memset(stat, 0, sizeof(*stat));
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = data_ ? data_->size() : 0;
        stat->grfMode = STGM_READ;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** clone) override {
        if (!clone) return STG_E_INVALIDPOINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        *clone = new (std::nothrow) SharedMemoryStream(data_, position_);
        return *clone ? S_OK : E_OUTOFMEMORY;
    }

private:
    ~SharedMemoryStream() = default;
    LONG refs_ = 1;
    std::shared_ptr<const std::vector<uint8_t>> data_;
    ULONGLONG position_ = 0;
    std::mutex mutex_;
};
}
