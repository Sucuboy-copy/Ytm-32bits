#pragma once
#include <windows.h>
#include <objidl.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include "ytm/pcm_buffer.hpp"

namespace ytm {
// IStream de solo lectura sobre un PcmBuffer. Read bloquea hasta que el decodificador haya escrito los bytes
// pedidos, de modo que Media Engine puede empezar a reproducir mientras el resto aún se descarga y decodifica.
// Los clones comparten el buffer y tienen posición propia.
class PcmStream final : public IStream {
public:
    explicit PcmStream(std::shared_ptr<PcmBuffer> buffer, ULONGLONG position = 0) : buffer_(std::move(buffer)), position_(position) {}

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
        ULONGLONG start;
        { std::lock_guard<std::mutex> lock(mutex_); start = position_; }
        size_t got = 0;
        const PcmBuffer::ReadResult result = buffer_->read((size_t)start, buffer, count, got);
        if (result == PcmBuffer::ReadResult::Cancelled) return E_ABORT;
        { std::lock_guard<std::mutex> lock(mutex_); position_ = start + got; }
        if (read) *read = (ULONG)got;
        return got == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPosition) override {
        std::lock_guard<std::mutex> lock(mutex_);
        LONGLONG base = 0;
        if (origin == STREAM_SEEK_SET) base = 0;
        else if (origin == STREAM_SEEK_CUR) base = (LONGLONG)position_;
        else if (origin == STREAM_SEEK_END) base = (LONGLONG)buffer_->total();
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
        stat->cbSize.QuadPart = buffer_->total();
        stat->grfMode = STGM_READ;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** clone) override {
        if (!clone) return STG_E_INVALIDPOINTER;
        ULONGLONG position;
        { std::lock_guard<std::mutex> lock(mutex_); position = position_; }
        *clone = new (std::nothrow) PcmStream(buffer_, position);
        return *clone ? S_OK : E_OUTOFMEMORY;
    }

private:
    ~PcmStream() = default;
    LONG refs_ = 1;
    std::shared_ptr<PcmBuffer> buffer_;
    ULONGLONG position_ = 0;
    std::mutex mutex_;
};
}
