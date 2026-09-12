#include "stealth.hpp"
#include "ring_signature.hpp"
#include "pedersen.hpp"
#include <cassert>
#include <iostream>
#include <cstring>

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "  [ERROR] " << msg << " (" #cond ")\n"; \
        return 1; \
    } \
} while(0)

int main() {
    std::cout << "[TEST] Iniciando prueba criptografica de privacidad estilo Monero...\n";
    if (sodium_init() < 0) {
        std::cerr << "Error al inicializar sodium\n";
        return 1;
    }

    // 1. Generar Billeteras para Alice (Emisora), Bob (Receptor) y Carol (Observadora Externa)
    auto alice = crypto::StealthWallet::generate_random();
    auto bob = crypto::StealthWallet::generate_random();
    auto carol = crypto::StealthWallet::generate_random();

    std::cout << "  -> Billeteras de Alice, Bob y Carol generadas.\n";
    std::cout << "  -> Direccion publica stealth de Bob: " << bob.get_public_address().encode().substr(0, 20) << "...\n";

    // 2. Alice crea un output de un solo uso para Bob
    crypto::Amount transfer_amount = 75 * crypto::USDT_UNIT;
    crypto::OneTimeOutput output_for_bob = crypto::StealthProtocol::create_one_time_output(
        bob.get_public_address(),
        transfer_amount
    );

    std::cout << "  -> Output de un solo uso generado en cadena: Destino P = "
              << crypto::to_hex(output_for_bob.destination_one_time).substr(0, 16) << "...\n";

    // 3. Carol (Observadora) escanea la blockchain: ¿le pertenece a Carol?
    bool carol_detected = crypto::StealthProtocol::scan_output(carol, output_for_bob);
    CHECK(!carol_detected, "Carol NO debe detectar el output de Bob");
    std::cout << "  -> Carol escanea el output: NO puede vincularlo (Privacidad preservada).\n";

    // 4. Bob (Receptor legítimo) escanea la blockchain: ¿le pertenece a Bob?
    bool bob_detected = crypto::StealthProtocol::scan_output(bob, output_for_bob);
    CHECK(bob_detected, "Bob DEBE detectar el output destinado a el");
    std::cout << "  -> Bob escanea el output: DETECTADO correctamente como propio.\n";

    // 5. Bob deriva su clave privada efímera de gasto
    crypto::Key256 one_time_priv = crypto::StealthProtocol::derive_one_time_private_key(bob, output_for_bob);

    // Verificar matemáticamente que x * G == P
    crypto::Key256 derived_P;
    crypto_scalarmult_ed25519_base_noclamp(derived_P.data(), one_time_priv.data());
    CHECK(std::memcmp(derived_P.data(), output_for_bob.destination_one_time.data(), 32) == 0, "x*G debe igualar P");
    std::cout << "  -> Bob derivo la clave privada de gasto: x * G == P verificado matematicamente.\n";

    // 6. Prueba de Firma de Anillo con Señuelos (Ring Signature LSAG)
    std::cout << "  -> Creando anillo de senuelos con tamano N = 5...\n";
    std::vector<crypto::Key256> ring;
    for (int i = 0; i < 4; ++i) {
        auto decoy_wallet = crypto::StealthWallet::generate_random();
        ring.push_back(decoy_wallet.spend_public_key);
    }
    // Insertar la clave real en la posición 2
    size_t real_index = 2;
    ring.insert(ring.begin() + real_index, output_for_bob.destination_one_time);

    crypto::Hash256 msg_hash;
    std::memset(msg_hash.data(), 0xAA, 32);

    auto ring_sig = crypto::RingSignatureEngine::sign(msg_hash, ring, real_index, one_time_priv);
    bool valid_sig = crypto::RingSignatureEngine::verify(msg_hash, ring_sig);
    CHECK(valid_sig, "Firma de anillo debe ser valida");
    std::cout << "  -> Firma de anillo verificada con exito (Imposible saber quien firmo en el anillo).\n";

    // 7. Prueba de Imagen de Clave y Prevención de Doble Gasto
    crypto::KeyImageLedger ledger;
    bool first_spend = ledger.register_key_image(ring_sig.key_image);
    CHECK(first_spend, "Primer gasto debe registrarse");
    std::cout << "  -> Primer gasto registrado correctamente en el ledger de Key Images.\n";

    bool second_spend = ledger.register_key_image(ring_sig.key_image);
    CHECK(!second_spend, "Doble gasto debe ser rechazado");
    std::cout << "  -> Intento de doble gasto rechazado exitosamente por Key Image repetida.\n";

    // 8. Prueba de Compromiso de Pedersen
    auto ped_in = crypto::Pedersen::commit(100 * crypto::USDT_UNIT);
    auto ped_out1 = crypto::Pedersen::commit(75 * crypto::USDT_UNIT);
    auto ped_out2 = crypto::Pedersen::commit(25 * crypto::USDT_UNIT);

    // Sum(r_in) - Sum(r_out)
    crypto::Key256 r_out_sum;
    crypto_core_ed25519_scalar_add(r_out_sum.data(), ped_out1.blinding_factor.data(), ped_out2.blinding_factor.data());
    crypto::Key256 r_diff;
    crypto_core_ed25519_scalar_sub(r_diff.data(), ped_in.blinding_factor.data(), r_out_sum.data());
    crypto::Key256 excess_pub;
    crypto_scalarmult_ed25519_base_noclamp(excess_pub.data(), r_diff.data());

    bool balance_ok = crypto::Pedersen::verify_balance_conservation(
        {ped_in.commitment_point},
        {ped_out1.commitment_point, ped_out2.commitment_point},
        0,
        excess_pub
    );
    CHECK(balance_ok, "Compromiso de Pedersen debe conservar balance");
    std::cout << "  -> Compromiso de Pedersen verificado: Sum(Inputs) == Sum(Outputs) en forma oculta.\n";

    std::cout << "[TEST PASSED] Todos los tests de privacidad estilo Monero superados con exito.\n";
    return 0;
}
