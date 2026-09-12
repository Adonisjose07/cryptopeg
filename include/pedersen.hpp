#pragma once

#include "types.hpp"

namespace crypto {

struct PedersenCommitment {
    Key256 commitment_point; // C = rG + vH
    Key256 blinding_factor;  // r (mantenido en secreto por el propietario)
    Amount value;            // v

    ~PedersenCommitment() {
        secure_wipe(blinding_factor);
    }
};

class Pedersen {
public:
    // Obtener el punto generador secundario H
    static const Key256& get_H();

    // Crear un compromiso C = rG + vH
    static PedersenCommitment commit(Amount amount);

    // Crear compromiso con un factor de cegado específico
    static Key256 commit_with_blinding(Amount amount, const Key256& blinding_factor);

    // Sumar dos compromisos: C3 = C1 + C2
    static Key256 add_commitments(const Key256& c1, const Key256& c2);

    // Restar dos compromisos: C3 = C1 - C2
    static Key256 sub_commitments(const Key256& c1, const Key256& c2);

    // Verificar balance confidencial: Sum(C_in) == Sum(C_out) + Fee*H + (Sum(r_in) - Sum(r_out))*G
    static bool verify_balance_conservation(
        const std::vector<Key256>& in_commitments,
        const std::vector<Key256>& out_commitments,
        Amount public_fee,
        const Key256& excess_blinding_pubkey // (Sum(r_in) - Sum(r_out))*G
    );
};

} // namespace crypto
