#include "node.hpp"
#include "rpc_server.hpp"
#include "p2p.hpp"
#include <iostream>
#include <csignal>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <sstream>

static std::unique_ptr<crypto::RpcServer> g_rpc_server;
static std::unique_ptr<crypto::P2PManager> g_p2p_manager;

void signal_handler(int signum) {
    std::cout << "\n[DAEMON] Senal (" << signum << ") capturada. Deteniendo P2P y servidor RPC...\n";
    if (g_p2p_manager) {
        g_p2p_manager->stop();
    }
    if (g_rpc_server) {
        g_rpc_server->stop();
    }
}

void run_demo(crypto::Node& node) {
    std::cout << "\n--- [EJECUTANDO DEMOSTRACION EN CADENA] ---\n";
    auto alice_wallet = crypto::StealthWallet::generate_random();
    auto bob_wallet = crypto::StealthWallet::generate_random();

    std::cout << "  * Direccion Stealth Alice: " << alice_wallet.get_public_address().encode().substr(0, 32) << "...\n";
    std::cout << "  * Direccion Stealth Bob  : " << bob_wallet.get_public_address().encode().substr(0, 32) << "...\n\n";

    crypto::Amount usdt_buy_amount = 1000 * crypto::USDT_UNIT;
    std::cout << "  -> Depositando " << crypto::format_usdt(usdt_buy_amount) << " para Alice...\n";
    auto deposit_receipt = node.buy_shielded(usdt_buy_amount, alice_wallet.get_public_address());

    crypto::OneTimeOutput alice_utxo;
    for (const auto& u : node.get_utxo_pool()) {
        if (crypto::StealthProtocol::scan_output(alice_wallet, u)) {
            alice_utxo = u;
            break;
        }
    }

    crypto::Amount send_to_bob = 350 * crypto::USDT_UNIT;
    std::cout << "  -> Enviando " << crypto::format_usdt(send_to_bob) << " de Alice a Bob de forma oculta...\n";
    node.transfer_shielded(alice_wallet, alice_utxo, bob_wallet.get_public_address(), send_to_bob, 0);

    crypto::OneTimeOutput bob_utxo;
    for (const auto& u : node.get_utxo_pool()) {
        if (crypto::StealthProtocol::scan_output(bob_wallet, u)) {
            bob_utxo = u;
            break;
        }
    }

    std::string bob_public_wallet = "0xBobExternalColdStorageWallet777";
    std::cout << "  -> Solicitando retiro anonimizado de Bob hacia " << bob_public_wallet << "...\n";
    node.withdraw_shielded(bob_wallet, bob_utxo, bob_utxo.amount, bob_public_wallet, true);

    std::cout << "  -> Auditoria de solvencia 1:1 post-demo: " << (node.audit_system() ? "OK" : "ERROR") << "\n\n";
}

int main(int argc, char* argv[]) {
    std::cout << "\n";
    std::cout << "========================================================================\n";
    std::cout << "   DAEMON CRIPTO PRIVADA: PARIDAD 1:1 USDT | MOTOR C++20 CON LMDB      \n";
    std::cout << "   DKSAP | RingCT Decoys | Fee Pool | Micro-Tumbler | REST API Daemon   \n";
    std::cout << "========================================================================\n\n";

    // Configuración desde entorno
    const char* env_db_dir = std::getenv("CRYPTO_DATA_DIR");
    std::string db_dir = env_db_dir ? env_db_dir : "./data/lmdb";

    const char* env_port = std::getenv("NODE_PORT");
    int port = env_port ? std::stoi(env_port) : 8080;

    // Configuración P2P desde argumentos y entorno
    std::vector<std::string> seed_peers;
    const char* env_peers = std::getenv("P2P_SEED_PEERS");
    if (env_peers) {
        std::istringstream iss(env_peers);
        std::string p;
        while (std::getline(iss, p, ',')) {
            if (!p.empty()) seed_peers.push_back(p);
        }
    }

    std::string node_id = "";
    const char* env_node_id = std::getenv("NODE_ID");
    if (env_node_id) node_id = env_node_id;

    std::string p2p_listen_url = "";
    const char* env_p2p_url = std::getenv("P2P_LISTEN_URL");
    if (env_p2p_url) {
        p2p_listen_url = env_p2p_url;
    } else {
        p2p_listen_url = "http://127.0.0.1:" + std::to_string(port);
    }

    bool demo_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--demo") {
            demo_mode = true;
        } else if (arg == "--peer" && i + 1 < argc) {
            seed_peers.push_back(argv[++i]);
        } else if (arg == "--node-id" && i + 1 < argc) {
            node_id = argv[++i];
        } else if (arg == "--p2p-url" && i + 1 < argc) {
            p2p_listen_url = argv[++i];
        }
    }
    if (std::getenv("RUN_DEMO") != nullptr) {
        demo_mode = true;
    }

    // Instalar manejadores de señales para Docker (SIGINT / SIGTERM)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Configuración de comisiones desde entorno (P1)
    uint32_t deposit_fee_bps = 50;
    const char* env_dep_fee = std::getenv("DEPOSIT_FEE_BPS");
    if (env_dep_fee) {
        try { deposit_fee_bps = static_cast<uint32_t>(std::stoul(env_dep_fee)); } catch (...) {}
    }
    uint32_t withdraw_fee_bps = 50;
    const char* env_wdr_fee = std::getenv("WITHDRAW_FEE_BPS");
    if (env_wdr_fee) {
        try { withdraw_fee_bps = static_cast<uint32_t>(std::stoul(env_wdr_fee)); } catch (...) {}
    }

    // Inicializar nodo con persistencia LMDB
    crypto::Node node(deposit_fee_bps, withdraw_fee_bps, db_dir);
    node.print_status();

    // Inicializar servicio de red P2P
    g_p2p_manager = std::make_unique<crypto::P2PManager>(node, p2p_listen_url, node_id);

    // Difundir bloques minados localmente vía Gossip
    node.set_on_block_mined([](const crypto::Block& b) {
        if (g_p2p_manager) {
            g_p2p_manager->broadcast_block(b);
        }
    });

    g_p2p_manager->start();

    // Conectar a nodos semilla iniciales
    for (const auto& peer : seed_peers) {
        std::cout << "[DAEMON] Conectando con par semilla P2P: " << peer << "\n";
        g_p2p_manager->add_peer(peer);
    }

    if (demo_mode) {
        run_demo(node);
        node.print_status();
    }

    // Iniciar servidor RPC en 0.0.0.0:8080
    std::cout << "[DAEMON] Inicializando endpoints de la API REST / JSON-RPC...\n";
    std::cout << "  -> GET  /api/v1/node/health       : Estado de salud del nodo\n";
    std::cout << "  -> GET  /api/v1/node/status       : Altura, solvencia 1:1 y reservas del pool\n";
    std::cout << "  -> GET  /api/v1/chain/blocks      : Lista de bloques minados en LMDB\n";
    std::cout << "  -> GET  /api/v1/chain/block/:h    : Detalle completo de bloque por altura\n";
    std::cout << "  -> POST /api/v1/wallet/generate   : Generar nueva billetera privada (DKSAP)\n";
    std::cout << "  -> POST /api/v1/wallet/scan       : Escaneo remoto de saldo con View-Key\n";
    std::cout << "  -> POST /api/v1/vault/deposit     : Depósito USDT -> Acuñación 1:1 a dirección furtiva\n";
    std::cout << "  -> POST /api/v1/tx/transfer       : Transferencia confidencial con anillo de señuelos\n";
    std::cout << "  -> POST /api/v1/vault/withdraw    : Retiro con dispersión atomizada del mezclador\n";
    std::cout << "  -> GET  /api/v1/p2p/status        : Estado de la red P2P y pares conectados\n";
    std::cout << "  -> POST /api/v1/p2p/handshake     : Negociación mutua de pares\n";
    std::cout << "  -> POST /api/v1/p2p/block         : Difusión Gossip de bloques\n";
    std::cout << "  -> GET  /api/v1/p2p/sync          : Sincronización rápida (IBD)\n\n";

    const char* env_host = std::getenv("NODE_HOST");
    std::string host = env_host ? env_host : "0.0.0.0";
    g_rpc_server = std::make_unique<crypto::RpcServer>(node, host, port);
    g_rpc_server->set_p2p_manager(g_p2p_manager.get());
    g_rpc_server->listen();

    std::cout << "[DAEMON] Servidor finalizado con exito. Base de datos LMDB sincronizada.\n";
    return 0;
}
