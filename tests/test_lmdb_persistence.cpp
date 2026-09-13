#include "node.hpp"
#include "types.hpp"
#include "stealth.hpp"
#include "block.hpp"
#include <iostream>
#include <cassert>
#include <filesystem>

void cleanup_test_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "  TEST: LMDB DISK PERSISTENCE & BLOCKCHAIN RESTORATION (RINGCT)  \n";
    std::cout << "=================================================================\n\n";

    const std::string test_db_path = "./test_data_lmdb";
    cleanup_test_dir(test_db_path);

    auto alice_wallet = crypto::StealthWallet::generate_random();
    auto bob_wallet = crypto::StealthWallet::generate_random();
    auto carol_wallet = crypto::StealthWallet::generate_random();

    crypto::KeyImage alice_spent_key_image{};
    crypto::OneTimeOutput alice_utxo{};
    crypto::OneTimeOutput bob_utxo{};

    // -------------------------------------------------------------
    // SESIÓN 1: Inicialización, Depósito y Transferencia Confidencial
    // -------------------------------------------------------------
    std::cout << "[SESION 1] Iniciando nodo con almacenamiento LMDB...\n";
    {
        crypto::Node node1(50, 50, test_db_path);
        assert(node1.get_blockchain_height() == 0);

        crypto::Block gen_block;
        assert(node1.get_block(0, gen_block));
        std::cout << "  [OK] Bloque Génesis verificado. Altura: " << gen_block.header.height
                  << " Hash: " << crypto::to_hex(gen_block.hash()).substr(0, 16) << "...\n";

        // Test de regresión adversarial AUD-RES-01: Intento de inyección con pool vacío
        {
            crypto::Block evil_block;
            evil_block.header.height = 1;
            evil_block.header.prev_block_hash = node1.get_top_block_hash();
            evil_block.header.timestamp = 1700000000;
            
            crypto::ShieldedTransaction evil_tx;
            evil_tx.public_fee = 0;
            evil_tx.ring_sig.ring_pubkeys = {alice_wallet.spend_public_key};
            crypto::OneTimeOutput evil_out{};
            evil_out.amount = 1000 * crypto::USDT_UNIT;
            evil_out.ephemeral_public_key = alice_wallet.view_public_key;
            evil_out.destination_one_time = alice_wallet.spend_public_key;
            evil_tx.outputs.push_back(evil_out);
            auto img = crypto::RingSignatureEngine::compute_key_image(alice_wallet.spend_private_key, alice_wallet.spend_public_key);
            evil_tx.ring_sig.key_image = img;
            evil_tx.tx_hash = crypto::RingSignatureEngine::compute_canonical_tx_hash(
                evil_tx.outputs, evil_tx.public_fee, evil_tx.ring_sig.ring_pubkeys, evil_tx.ring_sig.key_image
            );
            evil_tx.ring_sig = crypto::RingSignatureEngine::sign(
                evil_tx.tx_hash,
                evil_tx.ring_sig.ring_pubkeys,
                0,
                alice_wallet.spend_private_key
            );
            evil_block.txs.push_back(evil_tx);
            evil_block.header.merkle_root = evil_block.compute_merkle_root();

            std::string err;
            bool accepted = node1.apply_remote_block(evil_block, err);
            assert(!accepted);
            std::cout << "  [OK] Regresion AUD-RES-01 prevenida: bloque malicioso con pool vacio rechazado (" << err << ").\n";
        }

        // Alice deposita 1,000 USDT
        crypto::Amount deposit_gross = 1000 * crypto::USDT_UNIT;
        auto receipt = node1.buy_shielded(deposit_gross, alice_wallet.get_public_address());
        assert(node1.get_blockchain_height() == 1);
        std::cout << "  [OK] Deposito de Alice minado en Bloque 1.\n";

        // Alice escanea su UTXO
        bool found_alice = false;
        for (const auto& u : node1.get_utxo_pool()) {
            if (crypto::StealthProtocol::scan_output(alice_wallet, u)) {
                alice_utxo = u;
                found_alice = true;
                break;
            }
        }
        assert(found_alice);
        assert(alice_utxo.amount == receipt.net_shielded_tokens_minted);
        std::cout << "  [OK] Alice escaneó su salida privada de " << crypto::format_usdt(alice_utxo.amount) << "\n";

        // Alice transfiere 400 USDT a Bob
        crypto::Amount send_amount = 400 * crypto::USDT_UNIT;
        auto tx = node1.transfer_shielded(
            alice_wallet,
            alice_utxo,
            bob_wallet.get_public_address(),
            send_amount,
            0
        );
        assert(node1.get_blockchain_height() == 2);
        alice_spent_key_image = tx.ring_sig.key_image;
        std::cout << "  [OK] Transferencia Alice -> Bob minada en Bloque 2.\n";
        std::cout << "  [OK] Imagen de clave registrada: " << crypto::to_hex(alice_spent_key_image).substr(0, 16) << "...\n";

        // Bob escanea su UTXO
        bool found_bob = false;
        for (const auto& u : node1.get_utxo_pool()) {
            if (crypto::StealthProtocol::scan_output(bob_wallet, u)) {
                bob_utxo = u;
                found_bob = true;
                break;
            }
        }
        assert(found_bob);
        assert(bob_utxo.amount == send_amount);
        std::cout << "  [OK] Bob detectó sus 400 USDT.\n";

        assert(node1.audit_system());
        std::cout << "  [OK] Auditoría de solvencia 1:1 verificada en Sesión 1.\n";
        std::cout << "  -> Cerrando y destruyendo instancia del nodo (Simulando reinicio / shutdown)...\n\n";
    }

    // -------------------------------------------------------------
    // SESIÓN 2: Reinicio del Nodo y Verificación de Estado Persistido
    // -------------------------------------------------------------
    std::cout << "[SESION 2] Reiniciando nodo desde el mismo directorio LMDB...\n";
    {
        crypto::Node node2(50, 50, test_db_path);

        // 1. Verificar persistencia de altura de blockchain
        uint64_t recovered_height = node2.get_blockchain_height();
        std::cout << "  [OK] Altura recuperada de la cadena: " << recovered_height << " bloques (esperado: 2).\n";
        assert(recovered_height == 2);

        // 2. Verificar que los bloques individuales se leen de disco idénticos
        crypto::Block b1, b2;
        assert(node2.get_block(1, b1));
        assert(node2.get_block(2, b2));
        assert(b1.deposits.size() == 1);
        assert(b2.txs.size() == 1);
        assert(b2.header.prev_block_hash == b1.hash());
        std::cout << "  [OK] Encadenamiento criptográfico hash verificado en disco (Bloque 2 -> Bloque 1).\n";

        // 3. Verificar estado de solvencia y balances de la bóveda
        assert(node2.audit_system());
        assert(node2.get_vault().get_total_collateral() == 1000 * crypto::USDT_UNIT);
        assert(node2.get_vault().get_fee_pool_reserve() == 5 * crypto::USDT_UNIT);
        assert(node2.get_vault().get_circulating_shielded_supply() == 995 * crypto::USDT_UNIT);
        std::cout << "  [OK] Balances exactos de la bóveda restaurados: "
                  << crypto::format_usdt(node2.get_vault().get_total_collateral()) << " respaldados 1:1.\n";

        // 4. Verificar que Bob aún tiene y detecta sus fondos en el conjunto de UTXOs restaurado
        bool found_bob = false;
        crypto::OneTimeOutput bob_recovered_utxo{};
        for (const auto& u : node2.get_utxo_pool()) {
            if (crypto::StealthProtocol::scan_output(bob_wallet, u)) {
                bob_recovered_utxo = u;
                found_bob = true;
                break;
            }
        }
        assert(found_bob);
        assert(bob_recovered_utxo.amount == 400 * crypto::USDT_UNIT);
        std::cout << "  [OK] Bob recuperó con éxito su UTXO de 400 USDT tras el reinicio.\n";

        // 5. Verificar protección anti-doble gasto persistente:
        // Alice intenta gastar de nuevo el mismo output de la Sesión 1
        std::cout << "  -> Probando intento de doble gasto del output de Alice...\n";
        bool double_spend_caught = false;
        try {
            node2.transfer_shielded(
                alice_wallet,
                alice_utxo,
                carol_wallet.get_public_address(),
                100 * crypto::USDT_UNIT,
                0
            );
        } catch (const std::exception& ex) {
            double_spend_caught = true;
            std::cout << "  [OK] Doble gasto bloqueado correctamente por LMDB: " << ex.what() << "\n";
        }
        assert(double_spend_caught);

        // 6. Bob realiza una nueva transferencia a Carol en la Sesión 2
        std::cout << "  -> Bob transfiere 150 USDT a Carol en la Sesión 2...\n";
        crypto::Amount send_to_carol = 150 * crypto::USDT_UNIT;
        auto tx2 = node2.transfer_shielded(
            bob_wallet,
            bob_recovered_utxo,
            carol_wallet.get_public_address(),
            send_to_carol,
            0
        );
        assert(node2.get_blockchain_height() == 3);
        std::cout << "  [OK] Nueva transferencia minada en Bloque 3 tras reinicio.\n";

        // Carol escanea su UTXO
        bool found_carol = false;
        for (const auto& u : node2.get_utxo_pool()) {
            if (crypto::StealthProtocol::scan_output(carol_wallet, u)) {
                assert(u.amount == send_to_carol);
                found_carol = true;
                break;
            }
        }
        assert(found_carol);
        std::cout << "  [OK] Carol detectó sus 150 USDT transferidos por Bob.\n";
        assert(node2.audit_system());
    }

    cleanup_test_dir(test_db_path);
    std::cout << "\n=================================================================\n";
    std::cout << "  [EXITO TOTAL] Todas las pruebas de persistencia LMDB pasaron!  \n";
    std::cout << "=================================================================\n";
    return 0;
}
