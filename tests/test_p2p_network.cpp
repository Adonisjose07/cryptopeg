#include "p2p.hpp"
#include "node.hpp"
#include "rpc_server.hpp"
#include "stealth.hpp"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <thread>
#include <chrono>

void cleanup_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "  TEST SUITE: RED DESCENTRALIZADA P2P (GOSSIP & CHAIN SYNC)      \n";
    std::cout << "=================================================================\n\n";

    const std::string db_a = "./test_p2p_db_a";
    const std::string db_b = "./test_p2p_db_b";
    const std::string db_c = "./test_p2p_db_c";

    cleanup_dir(db_a);
    cleanup_dir(db_b);
    cleanup_dir(db_c);

    // -------------------------------------------------------------------------
    // PASO 1: Levantar Nodo Alpha (Port 8085) y Nodo Beta (Port 8086)
    // -------------------------------------------------------------------------
    std::cout << "[PASO 1] Iniciando Nodos validadores Alpha (8085) y Beta (8086)...\n";
    crypto::Node node_a(50, 50, db_a);
    crypto::Node node_b(50, 50, db_b);

    crypto::P2PManager p2p_a(node_a, "http://127.0.0.1:8085", "node-alpha");
    crypto::P2PManager p2p_b(node_b, "http://127.0.0.1:8086", "node-beta");

    // Conectar callbacks de difusión de bloques
    node_a.set_on_block_mined([&p2p_a](const crypto::Block& b) {
        p2p_a.broadcast_block(b);
    });
    node_b.set_on_block_mined([&p2p_b](const crypto::Block& b) {
        p2p_b.broadcast_block(b);
    });

    crypto::RpcServer rpc_a(node_a, "127.0.0.1", 8085);
    rpc_a.set_p2p_manager(&p2p_a);
    rpc_a.start_async();

    crypto::RpcServer rpc_b(node_b, "127.0.0.1", 8086);
    rpc_b.set_p2p_manager(&p2p_b);
    rpc_b.start_async();

    p2p_a.start();
    p2p_b.start();

    // -------------------------------------------------------------------------
    // PASO 2: Handshake P2P y Negociación de Pares
    // -------------------------------------------------------------------------
    std::cout << "[PASO 2] Ejecutando Handshake P2P entre Alpha y Beta...\n";
    bool hs_ok = p2p_a.add_peer("http://127.0.0.1:8086");
    assert(hs_ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    assert(p2p_a.get_peer_count() >= 1);
    assert(p2p_b.get_peer_count() >= 1);

    auto peers_a = p2p_a.get_active_peers();
    assert(!peers_a.empty() && peers_a[0].is_connected);
    assert(peers_a[0].node_id == "node-beta");
    std::cout << "  [OK] Handshake mutuo establecido con exito. Alpha conectado a Beta (" 
              << peers_a[0].latency_ms << "ms latencia).\n";

    // -------------------------------------------------------------------------
    // PASO 3: Propagación Gossip en Vivo de Bloque de Depósito
    // -------------------------------------------------------------------------
    std::cout << "\n[PASO 3] Probando difusión Gossip de nuevo bloque (Depósito en Alpha)...\n";
    auto alice_wallet = crypto::StealthWallet::generate_random();
    std::cout << "  -> Alice deposita 100 USDT en Nodo Alpha...\n";
    node_a.buy_shielded(100 * crypto::USDT_UNIT, alice_wallet.get_public_address());

    assert(node_a.get_blockchain_height() == 1);

    // Esperar propagación en red local
    for (int i = 0; i < 20 && node_b.get_blockchain_height() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    assert(node_b.get_blockchain_height() == 1);
    assert(crypto::to_hex(node_a.get_top_block_hash()) == crypto::to_hex(node_b.get_top_block_hash()));
    assert(node_b.get_vault().get_total_collateral() == 100 * crypto::USDT_UNIT);
    assert(node_b.audit_system());
    std::cout << "  [OK] Bloque #1 propagado por Gossip hacia Beta. Hash idéntico verificado: " 
              << crypto::to_hex(node_b.get_top_block_hash()).substr(0, 16) << "...\n";
    std::cout << "  [OK] Solvencia 1:1 verificada en Nodo Beta: " 
              << crypto::format_usdt(node_b.get_vault().get_total_collateral()) << "\n";

    // -------------------------------------------------------------------------
    // PASO 4: Propagación Gossip de Transacción Confidencial RingCT
    // -------------------------------------------------------------------------
    std::cout << "\n[PASO 4] Probando propagación Gossip de Transacción RingCT...\n";
    auto bob_wallet = crypto::StealthWallet::generate_random();

    // Localizar UTXO de Alice en Nodo Alpha
    crypto::OneTimeOutput alice_utxo;
    bool found_alice = false;
    for (const auto& u : node_a.get_utxo_pool()) {
        if (crypto::StealthProtocol::scan_output(alice_wallet, u)) {
            alice_utxo = u;
            found_alice = true;
            break;
        }
    }
    assert(found_alice);

    std::cout << "  -> Alice transfiere 35 USDT confidenciales a Bob en Nodo Alpha...\n";
    node_a.transfer_shielded(alice_wallet, alice_utxo, bob_wallet.get_public_address(), 35 * crypto::USDT_UNIT, 0);

    assert(node_a.get_blockchain_height() == 2);

    for (int i = 0; i < 20 && node_b.get_blockchain_height() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    assert(node_b.get_blockchain_height() == 2);
    assert(crypto::to_hex(node_a.get_top_block_hash()) == crypto::to_hex(node_b.get_top_block_hash()));
    std::cout << "  [OK] Bloque #2 (RingCT MLSAG) asimilado por Beta en consenso absoluto.\n";

    // Verificar que Bob puede detectar su saldo escaneando el Nodo Beta con su View-Key
    bool bob_found_on_beta = false;
    crypto::Amount bob_balance_on_beta = 0;
    for (const auto& u : node_b.get_utxo_pool()) {
        if (crypto::StealthProtocol::scan_output(bob_wallet, u)) {
            bob_found_on_beta = true;
            bob_balance_on_beta += u.amount;
        }
    }
    assert(bob_found_on_beta);
    assert(bob_balance_on_beta == 35 * crypto::USDT_UNIT);
    std::cout << "  [OK] Bob escaneó exitosamente el Nodo Beta remoto con su View-Key privada: " 
              << crypto::format_usdt(bob_balance_on_beta) << "\n";

    // -------------------------------------------------------------------------
    // PASO 5: Sincronización Inicial de Cadena (Initial Block Download - IBD)
    // -------------------------------------------------------------------------
    std::cout << "\n[PASO 5] Probando Initial Block Download (IBD) con nuevo Nodo Gamma (8087)...\n";
    crypto::Node node_c(50, 50, db_c);
    assert(node_c.get_blockchain_height() == 0); // Empieza solo con génesis

    crypto::P2PManager p2p_c(node_c, "http://127.0.0.1:8087", "node-gamma");
    crypto::RpcServer rpc_c(node_c, "127.0.0.1", 8087);
    rpc_c.set_p2p_manager(&p2p_c);
    rpc_c.start_async();
    p2p_c.start();

    // Conectar Gamma a Beta (que ya tiene altura 2)
    std::cout << "  -> Conectando Nodo Gamma a Nodo Beta...\n";
    p2p_c.add_peer("http://127.0.0.1:8086");

    // Sincronizar cadena desde Beta
    bool sync_ok = p2p_c.sync_from_peer("http://127.0.0.1:8086");
    assert(sync_ok);
    assert(node_c.get_blockchain_height() == 2);
    assert(crypto::to_hex(node_c.get_top_block_hash()) == crypto::to_hex(node_a.get_top_block_hash()));
    assert(node_c.audit_system());
    std::cout << "  [OK] Nodo Gamma descargó y validó todos los bloques históricos (Altura 0 -> 2).\n";
    std::cout << "  [OK] Consenso de 3 nodos alcanzado con 100% de coincidencia hash.\n";

    // -------------------------------------------------------------------------
    // Limpieza
    // -------------------------------------------------------------------------
    p2p_a.stop();
    p2p_b.stop();
    p2p_c.stop();

    rpc_a.stop();
    rpc_b.stop();
    rpc_c.stop();

    cleanup_dir(db_a);
    cleanup_dir(db_b);
    cleanup_dir(db_c);

    std::cout << "\n=================================================================\n";
    std::cout << "  >>> TODOS LOS TESTS DE RED P2P PASARON EXITOSAMENTE! <<<\n";
    std::cout << "=================================================================\n";

    return 0;
}
