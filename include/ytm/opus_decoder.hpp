#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "ytm/pcm_buffer.hpp"

namespace ytm {
// Descarga completa del WebM/Opus y decodificación a un contenedor WAV PCM de 16 bits en memoria
// (cabecera de 44 bytes + datos).
bool decodeWebmOpusToWav(const std::string& url, std::vector<uint8_t>& wav, std::string& diagnostic);

// Descarga por bloques y decodifica a `buffer` según llegan los datos. Llama a `onReady` (en el hilo que
// llama) cuando ya hay audio suficiente para empezar a reproducir; el resto sigue llenándose después.
// Si el contenedor no permite conocer el tamaño de antemano, descarga todo y entrega el buffer completo.
// Devuelve false con `diagnostic` si falla; `buffer` queda marcado como fallido (fin de archivo en lo escrito).
bool decodeWebmOpusStreaming(const std::string& url, const std::shared_ptr<PcmBuffer>& buffer,
                             const std::function<void()>& onReady, std::string& diagnostic);
}
