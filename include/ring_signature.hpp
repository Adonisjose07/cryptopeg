#pragma once

#include "types.hpp"
#include "stealth.hpp"
#include <vector>
#include <unordered_set>

namespace crypto {

struct RingSignature {
    KeyImage key_image;              // I = x * H_p(P)
    Key256 c0;                       // Desafío inicial c_0
    std::vector<Key256> responses;   // s_0, s_1, ..., s_{N-1}
    std::vector<Key256> ring_pubkeys;// Anillo de claves públicas P_0, ..., P_{N-1}
};

class RingSignatureEngine {
public:
    // Hash de punto a punto en la curva: H_p(P)
    static Key256 hash_to_point(const Key256& pubkey);

    // Calcular imagen de clave: I = x * H_p(P)
    static KeyImage compute_key_image(const Key256& privkey, const Key256& pubkey);

    // Computa el hash canónico completo de la transacción vinculando todas las salidas, comisión, anillo e imagen de clave (AUD-CRIT-03)
    static Hash256 compute_canonical_tx_hash(
        const std::vector<OneTimeOutput>& outputs,
        Amount public_fee,
        const std::vector<Key256>& ring_pubkeys,
        const KeyImage& key_image
    );

    // Firmar con anillo de señuelos (RingCT MLSAG)
    // message: hash de la transacción
    // ring: lista de N claves públicas (incluyendo señuelos y la real)
    // real_index: posición de la clave real en el anillo (0 <= real_index < N)
    // real_privkey: clave privada de un solo uso 'x'
    static RingSignature sign(
        const Hash256& message_hash,
        const std::vector<Key256>& ring,
        size_t real_index,
        const Key256& real_privkey
    );

    // Verificar firma de anillo (sin conocer cuál clave fue la firmante real)
    static bool verify(
        const Hash256& message_hash,
        const RingSignature& signature
    );
};

// Registro de imágenes de clave para prevenir doble-gasto
class KeyImageLedger {
public:
    // Retorna true si la imagen de clave es nueva y se registró con éxito; false si ya existe (doble gasto)
    bool register_key_image(const KeyImage& image);

    bool is_spent(const KeyImage& image) const;
    void restore_key_image(const KeyImage& image);

    size_t size() const { return spent_images_.size(); }

private:
    std::unordered_set<std::string> spent_images_;
};

} // namespace crypto
