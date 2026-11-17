#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ytm {
// Descarga un WebM/Opus y lo decodifica a un contenedor WAV PCM de 16 bits completo en memoria
// (cabecera de 44 bytes + datos), listo para entregarse a Media Engine sin pasar por disco.
bool decodeWebmOpusToWav(const std::string& url, std::vector<uint8_t>& wav, std::string& diagnostic);
}
