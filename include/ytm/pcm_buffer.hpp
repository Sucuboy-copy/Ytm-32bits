#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

namespace ytm {
// Contenedor WAV (cabecera + PCM) con tamaño final fijo que se va llenando desde un hilo escritor mientras
// otro hilo lo lee. La lectura bloquea hasta que los bytes pedidos existen, el escritor termina o se cancela.
// Lo que aún no se ha escrito es silencio (el vector se inicializa a cero), así que finish() solo marca el fin.
class PcmBuffer {
public:
    enum class ReadResult { Ok, Eof, Cancelled };

    // Reserva el tamaño final (cabecera de 44 bytes + PCM). Falla si no hay memoria o ya se canceló.
    bool begin(size_t totalBytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (begun_ || cancelled_) return false;
        try { data_.assign(totalBytes, 0); } catch (const std::bad_alloc&) { return false; }
        total_ = totalBytes; begun_ = true;
        cv_.notify_all();
        return true;
    }
    // Añade bytes al final de lo escrito. Lo que exceda el tamaño reservado se descarta.
    bool append(const void* src, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!begun_ || cancelled_) return false;
        const size_t n = std::min(count, total_ - written_);
        if (n) std::memcpy(data_.data() + written_, src, n);
        written_ += n;
        cv_.notify_all();
        return true;
    }
    // Entrega un contenedor ya completo (ruta de descarga completa).
    void adopt(std::vector<uint8_t>&& wav) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_) return;
        data_ = std::move(wav);
        total_ = written_ = data_.size();
        begun_ = done_ = true;
        cv_.notify_all();
    }
    // Fin correcto: lo que falte por escribir queda como silencio.
    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        written_ = total_; done_ = true;
        cv_.notify_all();
    }
    // Fin con error: los lectores ven fin de archivo justo donde se quedó lo escrito.
    void fail() {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
        cv_.notify_all();
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        cv_.notify_all();
    }
    bool cancelled() const { return cancelled_.load(); }
    size_t total() const { std::lock_guard<std::mutex> lock(mutex_); return total_; }
    size_t written() const { std::lock_guard<std::mutex> lock(mutex_); return written_; }

    ReadResult read(size_t pos, void* dst, size_t count, size_t& got) {
        got = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return cancelled_.load() || begun_; });
        if (cancelled_) return ReadResult::Cancelled;
        if (pos >= total_) return ReadResult::Eof;
        const size_t need = std::min(pos + count, total_);
        cv_.wait(lock, [&] { return cancelled_.load() || done_ || written_ >= need; });
        if (cancelled_) return ReadResult::Cancelled;
        if (pos >= written_) return ReadResult::Eof;
        got = std::min(count, written_ - pos);
        if (got) std::memcpy(dst, data_.data() + pos, got);
        return ReadResult::Ok;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<uint8_t> data_;
    size_t total_ = 0, written_ = 0;
    bool begun_ = false, done_ = false;
    std::atomic<bool> cancelled_{false};
};
}
