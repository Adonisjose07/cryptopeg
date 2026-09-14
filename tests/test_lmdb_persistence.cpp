#include "node.hpp"
#include "types.hpp"
#include "stealth.hpp"
#include "block.hpp"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <limits>

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

        // -------------------------------------------------------------
        // PASO 7 (AUD-H0-P0-01): Test Adversarial contra Salidas Huérfanas de Cambio
        // -------------------------------------------------------------
        std::cout << "\n  -> [TEST ADVERSARIAL P0-01] Inyección de salidas de cambio huérfanas en bloque...\n";
        crypto::Block malicious_change_block;
        malicious_change_block.header.height = node2.get_blockchain_height() + 1;
        malicious_change_block.header.prev_block_hash = node2.get_top_block_hash();
        malicious_change_block.header.timestamp = 1773276000ULL;

        // Inyectar salida de cambio huérfana de 5,000,000 USDT sin ningún retiro que la respalde
        auto dummy_wallet = crypto::StealthWallet::generate_random();
        auto orphan_change = crypto::StealthProtocol::create_one_time_output(dummy_wallet.get_public_address(), 5000000 * crypto::USDT_UNIT);
        malicious_change_block.withdrawal_outputs.push_back(orphan_change);
        malicious_change_block.header.merkle_root = malicious_change_block.compute_merkle_root();

        std::string err_p0_01;
        bool accepted_orphan = node2.apply_remote_block(malicious_change_block, err_p0_01);
        assert(!accepted_orphan);
        std::cout << "  [OK] Bloque con salida huérfana de cambio rechazado categóricamente: " << err_p0_01 << "\n";

        // -------------------------------------------------------------
        // PASO 8 (AUD-H0-P0-02): Test Adversarial contra Desbordamiento Aritmético de Amount
        // -------------------------------------------------------------
        std::cout << "\n  -> [TEST ADVERSARIAL P0-02] Inyección de transacción con wrap-around de Amount (UINT64_MAX)...\n";
        crypto::OneTimeOutput valid_utxo_for_ring = node2.get_utxo_pool().front();
        crypto::ShieldedTransaction overflow_tx;
        overflow_tx.public_fee = 0;
        crypto::OneTimeOutput of_out1 = crypto::StealthProtocol::create_one_time_output(dummy_wallet.get_public_address(), std::numeric_limits<crypto::Amount>::max() - 100);
        crypto::OneTimeOutput of_out2 = crypto::StealthProtocol::create_one_time_output(dummy_wallet.get_public_address(), 200);
        overflow_tx.outputs = {of_out1, of_out2};
        overflow_tx.ring_sig.ring_pubkeys = {valid_utxo_for_ring.destination_one_time};
        overflow_tx.ring_sig.key_image = dummy_wallet.spend_public_key;

        bool overflow_caught = false;
        try {
            node2.submit_pre_signed_transaction(overflow_tx);
        } catch (const std::exception& ex) {
            overflow_caught = true;
            std::string msg = ex.what();
            assert(msg.find("Amount overflow") != std::string::npos);
            std::cout << "  [OK] Desbordamiento aritmético capturado y bloqueado en submit: " << msg << "\n";
        }
        assert(overflow_caught);

        // Validar también en apply_remote_block
        crypto::Block overflow_block;
        overflow_block.header.height = node2.get_blockchain_height() + 1;
        overflow_block.header.prev_block_hash = node2.get_top_block_hash();
        overflow_block.header.timestamp = 1773276500ULL;
        // Hash canónico válido para superar el primer check
        overflow_tx.tx_hash = crypto::RingSignatureEngine::compute_canonical_tx_hash(
            overflow_tx.outputs,
            overflow_tx.public_fee,
            overflow_tx.ring_sig.ring_pubkeys,
            overflow_tx.ring_sig.key_image
        );
        overflow_block.txs.push_back(overflow_tx);
        overflow_block.header.merkle_root = overflow_block.compute_merkle_root();
        std::string err_overflow_block;
        bool accepted_overflow_block = node2.apply_remote_block(overflow_block, err_overflow_block);
        assert(!accepted_overflow_block);
        std::cout << "  [OK] Bloque con transacción desbordada rechazado en apply_remote_block: " << err_overflow_block << "\n";

        // -------------------------------------------------------------
        // PASO 9 (AUD-H0-P0-03): Test Adversarial contra Depósitos sin Firma de Validador Autorizado
        // -------------------------------------------------------------
        std::cout << "\n  -> [TEST ADVERSARIAL P0-03] Inyección de depósito P2P sin firma de validador autorizada...\n";
        crypto::Block fake_deposit_block;
        fake_deposit_block.header.height = node2.get_blockchain_height() + 1;
        fake_deposit_block.header.prev_block_hash = node2.get_top_block_hash();
        fake_deposit_block.header.timestamp = 1773277000ULL;

        crypto::DepositReceipt fake_dep;
        fake_dep.gross_usdt_deposited = 1000 * crypto::USDT_UNIT;
        fake_dep.fee_to_pool = 5 * crypto::USDT_UNIT;
        fake_dep.net_shielded_tokens_minted = 995 * crypto::USDT_UNIT;
        fake_dep.tx_hash = "0xfake_arbitrum_deposit_unbacked_hash";
        fake_dep.recipient_view_pub = dummy_wallet.view_public_key;
        fake_dep.recipient_spend_pub = dummy_wallet.spend_public_key;

        crypto::Hash256 dep_seed;
        crypto_generichash(dep_seed.data(), 32, reinterpret_cast<const uint8_t*>(fake_dep.tx_hash.data()), fake_dep.tx_hash.size(), nullptr, 0);
        auto fake_out = crypto::StealthProtocol::create_one_time_output(dummy_wallet.get_public_address(), fake_dep.net_shielded_tokens_minted, &dep_seed);

        fake_deposit_block.deposits.push_back(fake_dep);
        fake_deposit_block.deposit_outputs.push_back(fake_out);
        fake_deposit_block.header.merkle_root = fake_deposit_block.compute_merkle_root();
        // Encabezado no firmado
        fake_deposit_block.header.validator_pubkey.fill(0);
        fake_deposit_block.header.validator_signature.fill(0);

        std::string err_p0_03;
        bool accepted_fake_dep = node2.apply_remote_block(fake_deposit_block, err_p0_03);
        assert(!accepted_fake_dep);
        std::cout << "  [OK] Depósito P2P no autenticado rechazado categóricamente: " << err_p0_03 << "\n";

        // Configuramos lista blanca de validadores autorizados en node2
        node2.add_authorized_validator(node2.get_validator_pubkey());

        // Ahora firmamos con un validador no autorizado
        crypto::Key256 evil_pk;
        std::array<uint8_t, 64> evil_sk;
        crypto_sign_keypair(evil_pk.data(), evil_sk.data());
        fake_deposit_block.header.sign(evil_sk.data(), evil_pk);

        std::string err_unauth;
        bool accepted_unauth = node2.apply_remote_block(fake_deposit_block, err_unauth);
        assert(!accepted_unauth);
        std::cout << "  [OK] Depósito P2P firmado por validador no autorizado rechazado: " << err_unauth << "\n";

        // -> [TEST ADVERSARIAL P0-02] Modo Fail-Closed en lista blanca vacía
        std::cout << "\n  -> [TEST ADVERSARIAL P0-02] Modo Fail-Closed: Depósito P2P con lista de validadores vacía...\n";
        const std::string fail_closed_db = "./test_fail_closed_lmdb";
        cleanup_test_dir(fail_closed_db);
        crypto::Node fail_closed_node(50, 50, fail_closed_db);
        crypto::Block fail_closed_block = fake_deposit_block;
        fail_closed_block.header.height = fail_closed_node.get_blockchain_height() + 1;
        fail_closed_block.header.prev_block_hash = fail_closed_node.get_top_block_hash();
        std::string err_fc;
        bool fc_accepted = fail_closed_node.apply_remote_block(fail_closed_block, err_fc);
        assert(!fc_accepted);
        assert(err_fc.find("fail-closed") != std::string::npos);
        std::cout << "  [OK] Modo fail-closed activado: depósito rechazado ante lista de validadores vacía: " << err_fc << "\n";
        cleanup_test_dir(fail_closed_db);

        // -> [TEST ADVERSARIAL P1-01] Quórum Federado M-de-N
        std::cout << "\n  -> [TEST ADVERSARIAL P1-01] Quórum M-de-N (2-de-3 requerido)...\n";
        crypto::Key256 val1_pk, val2_pk, val3_pk;
        std::array<uint8_t, 64> val1_sk, val2_sk, val3_sk;
        crypto_sign_keypair(val1_pk.data(), val1_sk.data());
        crypto_sign_keypair(val2_pk.data(), val2_sk.data());
        crypto_sign_keypair(val3_pk.data(), val3_sk.data());

        node2.add_authorized_validator(val1_pk);
        node2.add_authorized_validator(val2_pk);
        node2.add_authorized_validator(val3_pk);
        node2.set_quorum_threshold(2); // Requiere 2 firmas independientes

        fake_deposit_block.header.sign(val1_sk.data(), val1_pk); // 1 sola firma
        std::string err_q1;
        bool q1_ok = node2.apply_remote_block(fake_deposit_block, err_q1);
        assert(!q1_ok);
        assert(err_q1.find("quórum insuficiente") != std::string::npos);
        std::cout << "  [OK] Bloque con 1 firma rechazado por quórum insuficiente: " << err_q1 << "\n";

        // Añadir segunda firma válida
        crypto::Signature64 sig2;
        crypto_sign_detached(sig2.data(), nullptr, fake_deposit_block.header.signing_hash().data(), 32, val2_sk.data());
        fake_deposit_block.header.add_quorum_signature(val2_pk, sig2);
        std::string err_q2;
        bool q2_ok = node2.apply_remote_block(fake_deposit_block, err_q2);
        assert(q2_ok);
        std::cout << "  [OK] Bloque con quórum 2-de-3 alcanzado aceptado y minado con éxito.\n";

        // -> [TEST ADVERSARIAL P1-02] Sustitución maliciosa de clave de cambio en retiro
        std::cout << "\n  -> [TEST ADVERSARIAL P1-02] Inyección de retiro con clave de cambio sustituida...\n";
        crypto::Block bad_change_block;
        bad_change_block.header.height = node2.get_blockchain_height() + 1;
        bad_change_block.header.prev_block_hash = node2.get_top_block_hash();
        bad_change_block.header.timestamp = 1773276000ULL;

        crypto::WithdrawalReceipt evil_wdr;
        evil_wdr.order_id = "ORD-EVIL-CHANGE-999";
        evil_wdr.gross_tokens_burned = 100 * crypto::USDT_UNIT;
        evil_wdr.fee_to_pool = 500'000ULL;
        evil_wdr.net_usdt_to_tumble = 99'500'000ULL;
        evil_wdr.destination_address = "0x1111222233334444555566667777888899990001";
        evil_wdr.burned_utxo_pubkey = fake_out.destination_one_time;

        // Clave legítima de cambio autorizada por el dueño
        crypto::Key256 legit_change_pk;
        randombytes_buf(legit_change_pk.data(), 32);
        evil_wdr.change_output_pubkey = legit_change_pk;

        // Firma DLEQ legítima del dueño sobre legit_change_pk
        crypto::Hash256 evil_burn_hash = crypto::RingSignatureEngine::compute_burn_message_hash(
            evil_wdr.order_id, evil_wdr.gross_tokens_burned, evil_wdr.destination_address, evil_wdr.change_output_pubkey
        );
        crypto::RingSignatureEngine::sign_burn_proof(
            evil_burn_hash, dummy_wallet.spend_public_key, dummy_wallet.spend_private_key,
            evil_wdr.key_image, evil_wdr.burn_signature_c0, evil_wdr.burn_signature_s
        );

        bad_change_block.withdrawals.push_back(evil_wdr);

        // El productor malicioso intenta desviar el cambio a un output válido controlado por el atacante.
        auto attacker_wallet = crypto::StealthWallet::generate_random();
        crypto::OneTimeOutput hijacked_change = crypto::StealthProtocol::create_one_time_output(
            attacker_wallet.get_public_address(),
            fake_out.amount - evil_wdr.gross_tokens_burned
        );
        bad_change_block.withdrawal_outputs.push_back(hijacked_change);

        bad_change_block.header.merkle_root = bad_change_block.compute_merkle_root();
        bad_change_block.header.sign(val1_sk.data(), val1_pk);
        std::string err_hijack;
        bool hijack_accepted = node2.apply_remote_block(bad_change_block, err_hijack);
        assert(!hijack_accepted);
        assert(err_hijack.find("P1-02") != std::string::npos || err_hijack.find("prueba DLEQ") != std::string::npos);
        std::cout << "  [OK] Sustitución maliciosa de cambio bloqueada categóricamente: " << err_hijack << "\n";

        // -> [TEST ADVERSARIAL P1-05] Regla Determinista Fork-Choice y Reorganización Canónica
        std::cout << "\n  -> [TEST ADVERSARIAL P1-05] Regla Fork-Choice y Reorganización de Punta (P1-05)...\n";
        uint64_t top_h = node2.get_blockchain_height();
        crypto::Hash256 orig_top_hash = node2.get_top_block_hash();

        // 1. Crear bloque competidor en la misma altura con menor quórum (1 firma vs 2 firmas del bloque local)
        crypto::Block competitor_low_q;
        competitor_low_q.header.height = top_h;
        crypto::Block current_top_blk;
        assert(node2.get_block(top_h, current_top_blk));
        competitor_low_q.header.prev_block_hash = current_top_blk.header.prev_block_hash;
        competitor_low_q.header.timestamp = current_top_blk.header.timestamp + 10;

        crypto::DepositReceipt comp_dep;
        comp_dep.gross_usdt_deposited = 500 * crypto::USDT_UNIT;
        comp_dep.fee_to_pool = 2500000ULL;
        comp_dep.net_shielded_tokens_minted = 497500000ULL;
        comp_dep.tx_hash = "0xcompetitor_tx_hash_alternative";
        comp_dep.recipient_view_pub = dummy_wallet.view_public_key;
        comp_dep.recipient_spend_pub = dummy_wallet.spend_public_key;

        crypto::Hash256 comp_seed;
        crypto_generichash(comp_seed.data(), 32, reinterpret_cast<const uint8_t*>(comp_dep.tx_hash.data()), comp_dep.tx_hash.size(), nullptr, 0);
        auto comp_out = crypto::StealthProtocol::create_one_time_output(dummy_wallet.get_public_address(), comp_dep.net_shielded_tokens_minted, &comp_seed);
        competitor_low_q.deposits.push_back(comp_dep);
        competitor_low_q.deposit_outputs.push_back(comp_out);
        competitor_low_q.header.merkle_root = competitor_low_q.compute_merkle_root();
        competitor_low_q.header.sign(val1_sk.data(), val1_pk); // Solo 1 firma

        std::string err_fc_low;
        bool low_q_acc = node2.apply_remote_block(competitor_low_q, err_fc_low);
        assert(!low_q_acc);
        assert(err_fc_low.find("Fork-Choice") != std::string::npos);
        assert(node2.get_top_block_hash() == orig_top_hash);
        std::cout << "  [OK] Bloque competidor con menor quórum rechazado por Fork-Choice: " << err_fc_low << "\n";

        // 2. Equipar al bloque competidor con MAYOR quórum (3 firmas vs 2 del local)
        crypto::Signature64 comp_sig2, comp_sig3;
        crypto_sign_detached(comp_sig2.data(), nullptr, competitor_low_q.header.signing_hash().data(), 32, val2_sk.data());
        crypto_sign_detached(comp_sig3.data(), nullptr, competitor_low_q.header.signing_hash().data(), 32, val3_sk.data());
        competitor_low_q.header.add_quorum_signature(val2_pk, comp_sig2);
        competitor_low_q.header.add_quorum_signature(val3_pk, comp_sig3);

        std::string err_fc_high;
        bool high_q_acc = node2.apply_remote_block(competitor_low_q, err_fc_high);
        assert(high_q_acc);
        assert(node2.get_blockchain_height() == top_h);
        assert(crypto::to_hex(node2.get_top_block_hash()) == crypto::to_hex(competitor_low_q.hash()));
        std::cout << "  [OK] Reorganización de punta completada por Fork-Choice (3 firmas > 2 firmas). Nuevo hash aceptado.\n";

        // 3. Probar buy_shielded con Quórum M-de-N (P1-01)
        std::cout << "\n  -> [TEST ADVERSARIAL P1-01] Node::buy_shielded con umbral de quórum >= 2...\n";
        bool threw_quorum = false;
        try {
            // Sin firmas adicionales cuando el umbral es 2 debe fallar
            node2.buy_shielded(100 * crypto::USDT_UNIT, dummy_wallet.get_public_address(), "0xinsufficient_quorum_tx");
        } catch (const std::exception& e) {
            threw_quorum = true;
            std::cout << "  [OK] buy_shielded rechazó minado sin quórum requerido: " << e.what() << "\n";
        }
        assert(threw_quorum);

        // Ahora pasando una segunda firma válida de cosignatario val2_sk
        auto bs_receipt = node2.buy_shielded(
            100 * crypto::USDT_UNIT,
            dummy_wallet.get_public_address(),
            "0xvalid_quorum_tx_deposit",
            0,
            {val2_sk}
        );
        assert(node2.get_blockchain_height() == top_h + 1);
        std::cout << "  [OK] buy_shielded con cosignatario adicional minó exitosamente Bloque #" << node2.get_blockchain_height() << "\n";

        // 4. Test Adversarial ReorgGuard: Bloque competidor con mayor quórum pero con validación fallida
        std::cout << "\n  -> [TEST ADVERSARIAL P1-05] ReorgGuard: Bloque competidor malicioso con mayor quórum pero inválido...\n";
        uint64_t safe_top_h = node2.get_blockchain_height();
        crypto::Hash256 safe_top_hash = node2.get_top_block_hash();
        crypto::Block safe_top_block;
        assert(node2.get_block(safe_top_h, safe_top_block));

        crypto::Amount expected_collateral_before = node2.get_vault().get_total_collateral();
        crypto::Amount expected_circulating_before = node2.get_vault().get_circulating_shielded_supply();
        crypto::Amount expected_fees_before = node2.get_vault().get_accumulated_fees();

        crypto::Block evil_reorg_block;
        evil_reorg_block.header.height = safe_top_h;
        evil_reorg_block.header.prev_block_hash = safe_top_block.header.prev_block_hash;
        evil_reorg_block.header.timestamp = safe_top_block.header.timestamp + 20;

        // Crear depósito malicioso sin salida correspondiente (para forzar fallo de validación)
        crypto::DepositReceipt evil_dep;
        evil_dep.gross_usdt_deposited = 1000 * crypto::USDT_UNIT;
        evil_dep.tx_hash = "0xevil_reorg_tx";
        evil_reorg_block.deposits.push_back(evil_dep);
        // NO agregamos deposit_output -> mismatch entre deposits y deposit_outputs
        evil_reorg_block.header.merkle_root = evil_reorg_block.compute_merkle_root();

        // Equipar con 3 firmas válidas (mayor quórum que el local)
        evil_reorg_block.header.sign(val1_sk.data(), val1_pk);
        crypto::Signature64 esig2, esig3;
        crypto_sign_detached(esig2.data(), nullptr, evil_reorg_block.header.signing_hash().data(), 32, val2_sk.data());
        crypto_sign_detached(esig3.data(), nullptr, evil_reorg_block.header.signing_hash().data(), 32, val3_sk.data());
        evil_reorg_block.header.add_quorum_signature(val2_pk, esig2);
        evil_reorg_block.header.add_quorum_signature(val3_pk, esig3);

        std::string err_evil_reorg;
        bool evil_reorg_accepted = node2.apply_remote_block(evil_reorg_block, err_evil_reorg);
        assert(!evil_reorg_accepted);
        std::cout << "  [OK] Bloque competidor inválido rechazado: " << err_evil_reorg << "\n";

        // Verificar que ReorgGuard restauró el bloque seguro localmente y su balance contable exacto (AUD-CP-01)
        assert(node2.get_blockchain_height() == safe_top_h);
        assert(crypto::to_hex(node2.get_top_block_hash()) == crypto::to_hex(safe_top_hash));
        assert(node2.get_vault().get_total_collateral() == expected_collateral_before);
        assert(node2.get_vault().get_circulating_shielded_supply() == expected_circulating_before);
        assert(node2.get_vault().get_accumulated_fees() == expected_fees_before);
        assert(node2.audit_system());
        std::cout << "  [OK] ReorgGuard restauró el bloque legítimo, la solvencia contable intacta (AUD-CP-01) y los UTXOs sin pérdida.\n";

        // -------------------------------------------------------------
        // PASO 10: Fork-Choice determinista con Merkle Root idéntico y hashes distintos (CP-AUD-01)
        // -------------------------------------------------------------
    std::cout << "\n[PASO 10] Fork-Choice determinista con Merkle Root idéntico y Hashes distintos (CP-AUD-01)...\n";
    {
        uint64_t cur_h = node2.get_blockchain_height();
        crypto::Block top_b;
        assert(node2.get_block_by_height(cur_h, top_b));

        // Construir un bloque competidor idéntico en transacciones/Merkle root pero firmado por val1_pk
        crypto::Block comp_block = top_b;
        comp_block.header.timestamp = top_b.header.timestamp + 5; // Modifica signing_hash
        comp_block.header.quorum_pubkeys.clear();
        comp_block.header.quorum_signatures.clear();
        comp_block.header.sign(val1_sk.data(), val1_pk); // 1 sola firma de quórum

        std::string err_fork;
        bool applied = node2.apply_remote_block(comp_block, err_fork);
        // Debe haber ejecutado Fork-Choice y no haber retornado 'true' ciegamente por Merkle root
        assert(!applied || crypto::to_hex(node2.get_top_block_hash()) == crypto::to_hex(comp_block.hash()));
        std::cout << "  [OK] Fork-Choice evaluó el bloque competidor sin falso cortocircuito por Merkle Root (CP-AUD-01).\n";
    }

    // -------------------------------------------------------------
    // PASO 11: Conservación estricta de balance: total_spent != ring_denomination (CP-MED-02)
    // -------------------------------------------------------------
    std::cout << "\n[PASO 11] Conservación Estricta de Balance: total_spent != ring_denomination (CP-MED-02)...\n";
    {
        crypto::Block def_block;
        def_block.header.height = node2.get_blockchain_height() + 1;
        def_block.header.prev_block_hash = node2.get_top_block_hash();
        def_block.header.timestamp = 1726000000ULL;

        auto pool = node2.get_utxo_pool();
        assert(!pool.empty());
        crypto::OneTimeOutput real_utxo = pool[0];

        // Crear transacción donde se gasta real_utxo.amount pero las salidas son menores (deflación / destrucción)
        crypto::ShieldedTransaction def_tx;
        def_tx.public_fee = 0;
        crypto::OneTimeOutput out;
        out.amount = (real_utxo.amount > 1000) ? (real_utxo.amount - 1000) : 1;
        randombytes_buf(out.ephemeral_public_key.data(), 32);
        randombytes_buf(out.destination_one_time.data(), 32);
        def_tx.outputs.push_back(out);

        def_tx.ring_sig.ring_pubkeys = {real_utxo.destination_one_time};
        def_tx.ring_sig.responses.resize(1);
        randombytes_buf(def_tx.ring_sig.responses[0].data(), 32);
        randombytes_buf(def_tx.ring_sig.c0.data(), 32);
        randombytes_buf(def_tx.ring_sig.key_image.data(), 32);

        def_block.txs.push_back(def_tx);
        def_block.header.merkle_root = def_block.compute_merkle_root();
        def_block.header.sign(val1_sk.data(), val1_pk);

        std::string err_def;
        bool def_applied = node2.apply_remote_block(def_block, err_def);
        assert(!def_applied);
        assert(err_def.find("conservacion de balance") != std::string::npos);
        std::cout << "  [OK] Transacción deflacionaria con destrucción de suministro rechazada categóricamente: " << err_def << "\n";
    }

    // -------------------------------------------------------------
    // PASO 12: Rechazo de Transacción con Claves Duplicadas en Anillo MLSAG (SEC-03)
    // -------------------------------------------------------------
    std::cout << "\n[PASO 12] Rechazo de Transacción con Claves Duplicadas en Anillo MLSAG (SEC-03)...\n";
    {
        crypto::Block dup_block;
        dup_block.header.height = node2.get_blockchain_height() + 1;
        dup_block.header.prev_block_hash = node2.get_top_block_hash();
        dup_block.header.timestamp = 1726000000ULL;

        auto pool = node2.get_utxo_pool();
        assert(!pool.empty());
        crypto::OneTimeOutput real_utxo = pool[0];

        crypto::ShieldedTransaction dup_tx;
        dup_tx.public_fee = 0;
        crypto::OneTimeOutput out;
        out.amount = real_utxo.amount;
        randombytes_buf(out.ephemeral_public_key.data(), 32);
        randombytes_buf(out.destination_one_time.data(), 32);
        dup_tx.outputs.push_back(out);

        // Anillo con clave duplicada [P, P]
        dup_tx.ring_sig.ring_pubkeys = {real_utxo.destination_one_time, real_utxo.destination_one_time};
        dup_tx.ring_sig.responses.resize(2);
        randombytes_buf(dup_tx.ring_sig.responses[0].data(), 32);
        randombytes_buf(dup_tx.ring_sig.responses[1].data(), 32);
        randombytes_buf(dup_tx.ring_sig.c0.data(), 32);
        randombytes_buf(dup_tx.ring_sig.key_image.data(), 32);

        dup_block.txs.push_back(dup_tx);
        dup_block.header.merkle_root = dup_block.compute_merkle_root();
        dup_block.header.sign(val1_sk.data(), val1_pk);

        std::string err_dup;
        bool dup_applied = node2.apply_remote_block(dup_block, err_dup);
        assert(!dup_applied);
        assert(err_dup.find("claves publicas duplicadas") != std::string::npos);
        std::cout << "  [OK] Bloque con claves duplicadas en anillo MLSAG rechazado categóricamente: " << err_dup << "\n";
    }
    }

    cleanup_test_dir(test_db_path);
    std::cout << "\n=================================================================\n";
    std::cout << "  [EXITO TOTAL] Todas las pruebas de persistencia LMDB pasaron!  \n";
    std::cout << "=================================================================\n";
    return 0;
}
