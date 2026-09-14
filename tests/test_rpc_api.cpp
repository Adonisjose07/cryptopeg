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
    crypto::Key256 val_pk;
    std::array<uint8_t, 64> val_sk;
    crypto_sign_keypair(val_pk.data(), val_sk.data());
    node.set_validator_key(val_sk.data(), val_pk);
    node.add_authorized_validator(val_pk);

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

    // 4. Test Deposit for Alice con Certificado Criptográfico (P0-01)
    std::cout << "[PASO 5] Probando POST /api/v1/vault/deposit con Certificado Criptográfico (P0-01)...\n";
    std::string tx_hash = "0xArbitrumSepoliaDepositTxAlice01";
    uint32_t log_idx = 0;
    crypto::Amount gross_raw = 1000 * crypto::USDT_UNIT;
    auto alice_stealth = crypto::StealthAddress::decode(alice_j["stealth_address"]);
    crypto::Hash256 att_hash = crypto::compute_deposit_attestation_hash(
        421614ULL, "0x511A31987EF1019a41CBba658935515Dd64d2D18",
        tx_hash, log_idx, gross_raw,
        alice_stealth.view_public_key, alice_stealth.spend_public_key, 100ULL
    );
    crypto::Signature64 att_sig;
    crypto_sign_detached(att_sig.data(), nullptr, att_hash.data(), 32, val_sk.data());

    json dep_req = {
        {"chain_id", 421614},
        {"contract_address", "0x511A31987EF1019a41CBba658935515Dd64d2D18"},
        {"gross_usdt_raw", std::to_string(gross_raw)},
        {"recipient_stealth_address", alice_j["stealth_address"]},
        {"tx_hash", tx_hash},
        {"log_index", log_idx},
        {"l2_block_number", 100},
        {"validator_pubkey", crypto::to_hex(val_pk)},
        {"validator_signature", crypto::to_hex(att_sig)}
    };

    // 4.1 Petición sin firma criptográfica debe ser rechazada con 401 (P0-01)
    json unauth_dep = dep_req;
    unauth_dep.erase("validator_signature");
    auto res_unauth = cli.Post("/api/v1/vault/deposit", unauth_dep.dump(), "application/json");
    assert(res_unauth && res_unauth->status == 401);
    std::cout << "  [OK] Depósito sin firma criptográfica rechazado con 401 (P0-01).\n";

    // 4.1b Petición que omite completamente ambos campos de validador (bypass check P0-01)
    json bypass_dep = dep_req;
    bypass_dep.erase("validator_pubkey");
    bypass_dep.erase("validator_signature");
    auto res_bypass = cli.Post("/api/v1/vault/deposit", bypass_dep.dump(), "application/json");
    assert(res_bypass && res_bypass->status == 401);
    std::cout << "  [OK] Depósito omitiendo credenciales rechazado con 401 (P0-01 bypass mitigado).\n";

    // 4.2 Petición simulada 0xCLI_ debe ser rechazada con 400 (P0-01)
    json fake_cli_dep = dep_req;
    fake_cli_dep["tx_hash"] = "0xCLI_fake_attempt";
    auto res_fake = cli.Post("/api/v1/vault/deposit", fake_cli_dep.dump(), "application/json");
    assert(res_fake && res_fake->status == 400);
    std::cout << "  [OK] Depósito simulado 0xCLI_ rechazado con 400 (P0-01).\n";

    // 4.3 Petición legítima con certificado Ed25519
    res = cli.Post("/api/v1/vault/deposit", dep_req.dump(), "application/json");
    assert(res && res->status == 200);
    auto dep_res = json::parse(res->body);
    assert(dep_res["success"] == true);
    assert(dep_res["block_height"] == 1);
    std::cout << "  [OK] Depósito procesado con certificado Ed25519. Minado en Bloque #1. Tokens: " << dep_res["net_shielded_minted"] << "\n";

    // 4.4 Probar Idempotencia: segundo intento con el mismo tx_hash/log_index debe retornar 409
    auto res_replay = cli.Post("/api/v1/vault/deposit", dep_req.dump(), "application/json");
    assert(res_replay && res_replay->status == 409);
    std::cout << "  [OK] Idempotencia compuesta verificada: intento de replay rechazado con código 409.\n";

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

    // 7b. Test Wallet Scan for Alice (Post-Transfer con filtrado de UTXOs gastados)
    std::cout << "[PASO 7b] Probando escaneo de Alice con spend_private_key para filtrar gastados...\n";
    json alice_post_scan_req = {
        {"view_private_key", alice_j["view_private_key"]},
        {"spend_public_key", alice_j["spend_public_key"]},
        {"spend_private_key", alice_j["spend_private_key"]}
    };
    res = cli.Post("/api/v1/wallet/scan", alice_post_scan_req.dump(), "application/json");
    assert(res && res->status == 200);
    auto alice_post_res = json::parse(res->body);
    assert(alice_post_res["outputs_count"] == 1);
    assert(alice_post_res["spent_outputs_count"] == 1);
    assert(alice_post_res["total_balance_units"] == 695000000ULL); // 995 - 300 = 695
    std::cout << "  [OK] Alice detectó cambio no gastado de " << alice_post_res["total_balance_usdt"] 
              << " y 1 salida gastada filtrada correctamente.\n";

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
        {"destination_public_usdt", "0xb0b0000000000000000000000000000000000001"}
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

    // 8b. Test Withdrawal rejection on invalid EVM address (AUD-CP-03 / QA-GAP-04)
    std::cout << "[PASO 9b] Probando rechazo de dirección EVM inválida en POST /api/v1/vault/withdraw...\n";
    json wdr_invalid = wdr_req;
    wdr_invalid["destination_public_usdt"] = "0xInvalidShortAddr";
    res = cli.Post("/api/v1/vault/withdraw", wdr_invalid.dump(), "application/json");
    assert(res && res->status == 400);
    std::cout << "  [OK] Retiro con dirección EVM inválida rechazado categóricamente con 400 Bad Request.\n";

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

    std::cout << "[PASO 12] Probando GET /api/v1/chain/block/3 (Detalle del bloque de retiro para el relayer)...\n";
    res = cli.Get("/api/v1/chain/block/3");
    assert(res && res->status == 200);
    auto b3_res = json::parse(res->body);
    assert(b3_res["height"] == 3);
    assert(b3_res["withdrawals"].size() == 1);
    assert(b3_res["withdrawals"][0]["destination"] == "0xb0b0000000000000000000000000000000000001");
    assert(b3_res["withdrawals"][0]["net_amount_raw"] == 298500000ULL);
    std::cout << "  [OK] Retiro verificado en bloque 3 con direccion de destino y monto raw exacto para el relayer L2.\n";

    // 9b. Test DoS limit capping (V4-07)
    std::cout << "[PASO 12b] Probando cota de seguridad contra DoS en GET /api/v1/chain/blocks?limit=999999...\n";
    res = cli.Get("/api/v1/chain/blocks?limit=999999");
    assert(res && res->status == 200);
    auto blocks_limit_res = json::parse(res->body);
    assert(blocks_limit_res["blocks"].size() <= 100);
    std::cout << "  [OK] Cota de paginación verificada contra DoS (V4-07).\n";

    // 9c. Test claim-fees authentication (V4-02)
    std::cout << "[PASO 12c] Probando autenticación administrativa en POST /api/v1/vault/claim-fees...\n";
    json claim_payload = {
        {"amount_usdt", 1.0},
        {"treasury_address", "0xTreasuryColdWallet"}
    };
    res = cli.Post("/api/v1/vault/claim-fees", claim_payload.dump(), "application/json");
    assert(res && res->status == 401);
    std::cout << "  [OK] Reclamo de comisiones no autenticado rechazado categóricamente con 401 Unauthorized (V4-02).\n";

    // 10. Apagado del Servidor
    std::cout << "[PASO 13] Deteniendo servidor RPC...\n";
    rpc.stop();
    assert(!rpc.is_running());
    std::cout << "  [OK] Servidor detenido limpiamente.\n";

    cleanup_dir(test_db);
    std::cout << "\n=================================================================\n";
    std::cout << "  [EXITO TOTAL] Todos los endpoints de la API REST pasaron!      \n";
    std::cout << "=================================================================\n";
    return 0;
}
