#pragma once

#include "types.hpp"
#include "stealth.hpp"
#include <string>
#include <vector>

namespace crypto {

class MnemonicEngine {
public:
    // Genera una frase semilla mnemónica de 24 palabras con 256 bits de entropía y checksum SHA-256
    static std::string generate_24_words();

    // Valida si una frase de 24 palabras es válida (palabras en el diccionario y checksum correcto)
    static bool validate_mnemonic(const std::string& phrase);

    // Convierte una frase mnemónica en una semilla determinista de 256 bits
    static Key256 mnemonic_to_entropy(const std::string& phrase);

    // Deriva deterministamente una StealthWallet (Spend Key y View Key) desde las 24 palabras
    static StealthWallet mnemonic_to_wallet(const std::string& phrase);

    // Divide una cadena mnemónica en lista de palabras individuales
    static std::vector<std::string> split_words(const std::string& phrase);
};

} // namespace crypto
