#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sodium.h>

namespace crypto {

// Unidad atómica: micro-USDT (1 USDT = 1,000,000 unidades)
// Exactamente como el estándar TRC-20 y ERC-20 de USDT (6 decimales).
using Amount = uint64_t;
constexpr Amount USDT_UNIT = 1'000'000ULL;

using Key256 = std::array<uint8_t, 32>;
using Hash256 = std::array<uint8_t, 32>;
using KeyImage = std::array<uint8_t, 32>;

// Borrado seguro de memoria RAM para claves privadas
inline void secure_wipe(void* ptr, size_t len) {
    if (ptr && len > 0) {
        sodium_memzero(ptr, len);
    }
}

template <size_t N>
inline void secure_wipe(std::array<uint8_t, N>& arr) {
    sodium_memzero(arr.data(), N);
}

// Conversión a cadena Hexadecimal
inline std::string to_hex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return oss.str();
}

template <size_t N>
inline std::string to_hex(const std::array<uint8_t, N>& arr) {
    return to_hex(arr.data(), N);
}

inline std::vector<uint8_t> from_hex(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        throw std::invalid_argument("Cadena hexadecimal con longitud inválida.");
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

// Formateo financiero exacto (sin punto flotante)
inline std::string format_usdt(Amount micro_usdt) {
    uint64_t whole = micro_usdt / USDT_UNIT;
    uint64_t frac = micro_usdt % USDT_UNIT;
    std::ostringstream oss;
    oss << whole << "." << std::setw(6) << std::setfill('0') << frac << " USDT";
    return oss.str();
}

inline Amount parse_usdt(double usdt) {
    if (usdt < 0.0) {
        throw std::invalid_argument("El monto en USDT no puede ser negativo.");
    }
    return static_cast<Amount>(usdt * 1'000'000.0 + 0.5);
}

} // namespace crypto
