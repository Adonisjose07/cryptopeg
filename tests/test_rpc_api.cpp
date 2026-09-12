#include "rpc_server.hpp"
#include "node.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <cassert>
#include <filesystem>

using json = nlohmann::json;

void cleanup_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "  TEST: DAEMON HTTP/REST & JSON-RPC API SERVER (PORT 8081)       \n";
    std::cout << "=================================================================\n\n";

    const std::string test_db = "./test_rpc_lmdb";
    cleanup_dir(test_db);

    crypto::Node node(50, 50, test_db);
    crypto::RpcServer rpc(node, "127.0.0.1", 8081);

    std::cout << "[PASO 1] Iniciando RpcServer asíncrono en http://127.0.0.1:8081...\n";
    rpc.start_async();
    assert(rpc.is_running());

    httplib::Client cli("http://127.0.0.1:8081");
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);

    // 1. Test Health
    std::cout << "[PASO 2] Probando GET /api/v1/node/health...\n";
    auto res = cli.Get("/api/v1/node/health");
    assert(res && res->status == 200);
    auto j_health = json::parse(res->body);
    assert(j_health["status"] == "ok");
    assert(j_health["version"] == "1.0.0");
    std::cout << "  [OK] Health check responde 200 OK.\n";

    // 2. Test Node Status
    std::cout << "[PASO 3] Probando GET /api/v1/node/status...\n";
    res = cli.Get("/api/v1/node/status");
    assert(res && res->status == 200);
    auto j_status = json::parse(res->body);
    assert(j_status["blockchain_height"] == 0);
    assert(j_status["vault"]["is_solvent_1_to_1"] == true);
    std::cout << "  [OK] Estado inicial verificado: Bloque 0 (Génesis), solvencia válida.\n";

    // 3. Test Generate Wallets for Alice and Bob
    std::cout << "[PASO 4] Probando POST /api/v1/wallet/generate para Alice y Bob...\n";
    res = cli.Post("/api/v1/wallet/generate", "", "application/json");
    assert(res && res->status == 200);
    auto alice_j = json::parse(res->body);

    res = cli.Post("/api/v1/wallet/generate", "", "application/json");
    assert(res && res->status == 200);
    auto bob_j = json::parse(res->body);

    std::cout << "  [OK] Billetera Alice generada: " << alice_j["stealth_address"].get<std::string>().substr(0, 24) << "...\n";
    std::cout << "  [OK] Billetera Bob generada  : " << bob_j["stealth_address"].get<std::string>().substr(0, 24) << "...\n";

    // 4. Test Deposit for Alice
    std::cout << "[PASO 5] Probando POST /api/v1/vault/deposit (1,000 USDT para Alice)...\n";
    json dep_req = {
        {"gross_usdt", 1000.0},
        {"recipient_stealth_address", alice_j["stealth_address"]}
    };
    res = cli.Post("/api/v1/vault/deposit", dep_req.dump(), "application/json");
    assert(res && res->status == 200);
    auto dep_res = json::parse(res->body);
    assert(dep_res["success"] == true);
    assert(dep_res["block_height"] == 1);
    std::cout << "  [OK] Depósito procesado. Minado en Bloque #1. Tokens acuñados: " << dep_res["net_shielded_minted"] << "\n";

    // 5. Test Wallet Scan for Alice (View-Key Scanning)
    std::cout << "[PASO 6] Probando POST /api/v1/wallet/scan (Escaneo con View-Key de Alice)...\n";
    json scan_req = {
        {"view_private_key", alice_j["view_private_key"]},
        {"spend_public_key", alice_j["spend_public_key"]}
    };
    res = cli.Post("/api/v1/wallet/scan", scan_req.dump(), "application/json");
    assert(res && res->status == 200);
    auto scan_res = json::parse(res->body);
    assert(scan_res["outputs_count"] == 1);
    assert(scan_res["total_balance_units"] == 995000000ULL);
    std::string alice_utxo_pub = scan_res["outputs"][0]["destination_one_time"];
    std::cout << "  [OK] Alice escaneó su saldo privado: " << scan_res["total_balance_usdt"] << "\n";

    // 6. Test Shielded Transfer Alice -> Bob (300 USDT)
    std::cout << "[PASO 7] Probando POST /api/v1/tx/transfer (Alice envía 300 USDT a Bob)...\n";
    json tx_req = {
        {"sender_spend_private_key", alice_j["spend_private_key"]},
        {"sender_view_private_key", alice_j["view_private_key"]},
        {"input_utxo_pubkey", alice_utxo_pub},
        {"recipient_stealth_address", bob_j["stealth_address"]},
        {"amount_usdt", 300.0},
        {"tx_fee_usdt", 0.0}
    };
    res = cli.Post("/api/v1/tx/transfer", tx_req.dump(), "application/json");
    assert(res && res->status == 200);
    auto tx_res = json::parse(res->body);
    assert(tx_res["success"] == true);
    assert(tx_res["block_height"] == 2);
    assert(tx_res["ring_size"] == 5);
    std::cout << "  [OK] Transferencia privada minada en Bloque #2. Ring Size: " << tx_res["ring_size"] << "\n";

    // 7. Test Wallet Scan for Bob
    std::cout << "[PASO 8] Probando POST /api/v1/wallet/scan para Bob...\n";
    json bob_scan_req = {
        {"view_private_key", bob_j["view_private_key"]},
        {"spend_public_key", bob_j["spend_public_key"]}
    };
    res = cli.Post("/api/v1/wallet/scan", bob_scan_req.dump(), "application/json");
    assert(res && res->status == 200);
    auto bob_scan_res = json::parse(res->body);
    assert(bob_scan_res["outputs_count"] == 1);
    assert(bob_scan_res["total_balance_units"] == 300000000ULL);
    std::string bob_utxo_pub = bob_scan_res["outputs"][0]["destination_one_time"];
    std::cout << "  [OK] Bob detectó sus 300 USDT entrantes.\n";

    // 8. Test Withdrawal with Micro-Tumbler for Bob
    std::cout << "[PASO 9] Probando POST /api/v1/vault/withdraw (Bob retira a cold storage)...\n";
    json wdr_req = {
        {"burner_spend_private_key", bob_j["spend_private_key"]},
        {"burner_view_private_key", bob_j["view_private_key"]},
        {"input_utxo_pubkey", bob_utxo_pub},
        {"tokens_to_withdraw", 300.0},
        {"destination_public_usdt", "0xBobExternalColdStorageWallet999"}
    };
    res = cli.Post("/api/v1/vault/withdraw", wdr_req.dump(), "application/json");
    assert(res && res->status == 200);
    auto wdr_res = json::parse(res->body);
    assert(wdr_res["success"] == true);
    assert(wdr_res["block_height"] == 3);
    assert(wdr_res["total_micro_fragments"].get<int>() > 0);
    std::cout << "  [OK] Retiro procesado y minado en Bloque #3.\n";
    std::cout << "  -> Orden: " << wdr_res["order_id"] << "\n";
    std::cout << "  -> Micro-fragmentos anonimizados generados: " << wdr_res["total_micro_fragments"] << "\n";

    // 9. Test Chain Exploration
    std::cout << "[PASO 10] Probando GET /api/v1/chain/blocks...\n";
    res = cli.Get("/api/v1/chain/blocks?limit=10");
    assert(res && res->status == 200);
    auto blocks_res = json::parse(res->body);
    assert(blocks_res["total_height"] == 3);
    assert(blocks_res["blocks"].size() == 4); // 0, 1, 2, 3
    std::cout << "  [OK] Lista de bloques recuperada. Total bloques: " << blocks_res["blocks"].size() << "\n";

    std::cout << "[PASO 11] Probando GET /api/v1/chain/block/2 (Detalle del bloque de transferencia)...\n";
    res = cli.Get("/api/v1/chain/block/2");
    assert(res && res->status == 200);
    auto b2_res = json::parse(res->body);
    assert(b2_res["height"] == 2);
    assert(b2_res["transactions"].size() == 1);
    std::cout << "  [OK] Detalle del bloque 2 verificado con su transacción de anillo.\n";

    // 10. Apagado del Servidor
    std::cout << "[PASO 12] Deteniendo servidor RPC...\n";
    rpc.stop();
    assert(!rpc.is_running());
    std::cout << "  [OK] Servidor detenido limpiamente.\n";

    cleanup_dir(test_db);
    std::cout << "\n=================================================================\n";
    std::cout << "  [EXITO TOTAL] Todos los endpoints de la API REST pasaron!      \n";
    std::cout << "=================================================================\n";
    return 0;
}
