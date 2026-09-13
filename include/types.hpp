#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <cmath>
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
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            throw std::invalid_argument("Carácter hexadecimal inválido.");
        }
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        size_t idx = 0;
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, &idx, 16));
        if (idx != 2) {
            throw std::invalid_argument("Error parseando byte hexadecimal.");
        }
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
    if (std::isnan(usdt) || std::isinf(usdt) || usdt < 0.0 || usdt > 1e12) {
        throw std::invalid_argument("Monto en USDT inválido o fuera de rango (AUD-MED-01).");
    }
    return static_cast<Amount>(usdt * 1'000'000.0 + 0.5);
}

// Representante canónico en el subgrupo primo multiplicando por el cofactor 8 (AUD-CRIT-01)
// Se calcula mediante 3 duplicaciones sucesivas de punto en curva (2P, 4P, 8P) con adición completa de Edwards.
// Garantiza que cualquier punto I y todos sus alias de torsión (I + Tk) colapsen al mismo punto canónico 8*I.
inline KeyImage canonical_key_image(const KeyImage& image) {
    KeyImage p2, p4, p8;
    if (crypto_core_ed25519_add(p2.data(), image.data(), image.data()) != 0) return image;
    if (crypto_core_ed25519_add(p4.data(), p2.data(), p2.data()) != 0) return image;
    if (crypto_core_ed25519_add(p8.data(), p4.data(), p4.data()) != 0) return image;
    return p8;
}

} // namespace crypto
