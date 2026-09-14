#pragma once

#include "types.hpp"
#include <vector>
#include <string>
#include <cstring>
#include <stdexcept>
#include <array>
#include <limits>

namespace crypto {

// Límite protocolario para cadenas recibidas desde red. Evita reservas/copies
// desproporcionadas antes de que el bloque sea validado semánticamente.
constexpr uint32_t MAX_SERIALIZED_STRING_BYTES = 64 * 1024;

class ByteWriter {
public:
    void write_u8(uint8_t val) {
        buffer_.push_back(val);
    }

    void write_u32(uint32_t val) {
        for (int i = 0; i < 4; ++i) {
            buffer_.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }
    }

    void write_u64(uint64_t val) {
        for (int i = 0; i < 8; ++i) {
            buffer_.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }
    }

    void write_bytes(const uint8_t* data, size_t len) {
        if (len > 0) {
            buffer_.insert(buffer_.end(), data, data + len);
        }
    }

    template <size_t N>
    void write_array(const std::array<uint8_t, N>& arr) {
        write_bytes(arr.data(), N);
    }

    void write_string(const std::string& str) {
        if (str.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Cadena demasiado grande para serializacion u32.");
        }
        write_u32(static_cast<uint32_t>(str.size()));
        if (!str.empty()) {
            write_bytes(reinterpret_cast<const uint8_t*>(str.data()), str.size());
        }
    }

    const std::vector<uint8_t>& get_bytes() const { return buffer_; }
    std::vector<uint8_t> take_bytes() { return std::move(buffer_); }
    size_t size() const { return buffer_.size(); }

private:
    std::vector<uint8_t> buffer_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), cursor_(0) {}

    uint8_t read_u8() {
        require_remaining(1, "u8");
        return data_[cursor_++];
    }

    uint32_t read_u32() {
        require_remaining(4, "u32");
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i) {
            val |= (static_cast<uint32_t>(data_[cursor_++]) << (i * 8));
        }
        return val;
    }

    uint64_t read_u64() {
        require_remaining(8, "u64");
        uint64_t val = 0;
        for (int i = 0; i < 8; ++i) {
            val |= (static_cast<uint64_t>(data_[cursor_++]) << (i * 8));
        }
        return val;
    }

    void read_bytes(uint8_t* out, size_t len) {
        require_remaining(len, "bytes");
        std::memcpy(out, data_ + cursor_, len);
        cursor_ += len;
    }

    template <size_t N>
    std::array<uint8_t, N> read_array() {
        std::array<uint8_t, N> arr;
        read_bytes(arr.data(), N);
        return arr;
    }

    std::string read_string() {
        uint32_t len = read_u32();
        if (len > MAX_SERIALIZED_STRING_BYTES) {
            throw std::runtime_error("Cadena serializada excede el limite protocolario de seguridad.");
        }
        require_remaining(static_cast<size_t>(len), "cadena");
        std::string str(reinterpret_cast<const char*>(data_ + cursor_), len);
        cursor_ += len;
        return str;
    }

    size_t remaining() const {
        return (cursor_ <= size_) ? (size_ - cursor_) : 0;
    }

private:
    void require_remaining(size_t len, const char* what) const {
        // Comparar contra remaining evita overflow de cursor_ + len.
        if (cursor_ > size_ || len > size_ - cursor_) {
            throw std::runtime_error(std::string("Desbordamiento al leer ") + what + " en deserializacion.");
        }
    }

    const uint8_t* data_;
    size_t size_;
    size_t cursor_;
};

// Conversión Big-Endian para claves de altura LMDB (orden lexicográfico idéntico al numérico)
inline uint64_t to_big_endian_64(uint64_t val) {
    return ((val & 0xFF00000000000000ULL) >> 56) |
           ((val & 0x00FF000000000000ULL) >> 40) |
           ((val & 0x0000FF0000000000ULL) >> 24) |
           ((val & 0x000000FF00000000ULL) >> 8)  |
           ((val & 0x00000000FF000000ULL) << 8)  |
           ((val & 0x0000000000FF0000ULL) << 24) |
           ((val & 0x000000000000FF00ULL) << 40) |
           ((val & 0x00000000000000FFULL) << 56);
}

inline uint64_t from_big_endian_64(uint64_t val) {
    return to_big_endian_64(val);
}

} // namespace crypto
