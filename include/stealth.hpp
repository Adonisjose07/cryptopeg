#pragma once

#include "types.hpp"
#include <string>

namespace crypto {

struct StealthAddress {
    Key256 spend_public_key; // Clave pública de gasto (A = aG)
    Key256 view_public_key;  // Clave pública de vista (B = bG)

    std::string encode() const;
    static StealthAddress decode(const std::string& encoded);
};

struct StealthWallet {
    Key256 spend_private_key; // a
    Key256 spend_public_key;  // A
    Key256 view_private_key;  // b
    Key256 view_public_key;   // B

    ~StealthWallet() {
        secure_wipe(spend_private_key);
        secure_wipe(view_private_key);
    }

    StealthAddress get_public_address() const {
        return StealthAddress{spend_public_key, view_public_key};
    }

    static StealthWallet generate_random();
};

struct OneTimeOutput {
    Key256 ephemeral_public_key; // R = rG
    Key256 destination_one_time; // P = H(rB)G + A
    Amount amount;               // Monto (o compromiso)
};

class StealthProtocol {
public:
    // El emisor crea un output de un solo uso para la dirección del receptor (aleatorio o determinista si hay semilla)
    static OneTimeOutput create_one_time_output(
        const StealthAddress& recipient_address,
        Amount amount,
        const Hash256* deterministic_seed = nullptr
    );

    // El receptor escanea un output en la blockchain para ver si le pertenece
    static bool scan_output(
        const StealthWallet& recipient_wallet,
        const OneTimeOutput& output
    );

    // Si le pertenece, deriva la clave privada de un solo uso para gastarlo: x = H(b R) + a
    static Key256 derive_one_time_private_key(
        const StealthWallet& recipient_wallet,
        const OneTimeOutput& output
    );
};

} // namespace crypto
