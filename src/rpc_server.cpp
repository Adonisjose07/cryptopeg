#include "rpc_server.hpp"
#include "p2p.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstring>

using json = nlohmann::json;

namespace crypto {

RpcServer::RpcServer(Node& node, const std::string& host, int port)
    : node_(node), host_(host), port_(port) {
    server_ = std::make_unique<httplib::Server>();
    server_->set_payload_max_length(5 * 1024 * 1024); // 5 MB máx (V4-06)
    server_->set_read_timeout(5, 0);                  // 5 segundos timeout lectura
    server_->set_write_timeout(5, 0);                 // 5 segundos timeout escritura
    setup_routes();
}

RpcServer::~RpcServer() {
    stop();
}

void RpcServer::listen() {
    if (!server_) return;
    is_running_ = true;
    std::cout << "[RPC SERVER] Servidor escuchando en http://" << host_ << ":" << port_ << "\n";
    server_->listen(host_.c_str(), port_);
    is_running_ = false;
}

void RpcServer::start_async() {
    if (is_running_) return;
    worker_thread_ = std::thread([this]() {
        listen();
    });
    // Esperar a que el servidor esté activo
    while (!server_->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    is_running_ = true;
}

void RpcServer::stop() {
    if (server_ && server_->is_running()) {
        server_->stop();
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    is_running_ = false;
}

void RpcServer::setup_routes() {
    // Configuración de cabeceras CORS seguras (V4-11)
    const char* env_cors = std::getenv("CORS_ALLOWED_ORIGIN");
    std::string cors_origin = env_cors ? env_cors : "http://localhost:8080";
    server_->set_default_headers({
        {"Access-Control-Allow-Origin", cors_origin},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, X-Admin-Token"}
    });

    server_->Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
    });

    // Servir archivos estáticos del frontend
    if (std::filesystem::exists("./public")) {
        server_->set_mount_point("/", "./public");
    } else if (std::filesystem::exists("/app/public")) {
        server_->set_mount_point("/", "/app/public");
    }

    server_->Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::string p1 = "./public/index.html";
        std::string p2 = "/app/public/index.html";
        std::string path = std::filesystem::exists(p1) ? p1 : (std::filesystem::exists(p2) ? p2 : "");
        if (!path.empty()) {
            std::ifstream ifs(path);
            if (ifs.is_open()) {
                std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
                res.set_content(content, "text/html; charset=utf-8");
                return;
            }
        }
        res.set_content("<html><body><h1>CryptoPegUSDT Node Daemon</h1><p>API activa en /api/v1/node/status</p></body></html>", "text/html; charset=utf-8");
    });

    // 1. Health Check
    server_->Get("/api/v1/node/health", [](const httplib::Request&, httplib::Response& res) {
        json j = {
            {"status", "ok"},
            {"version", "1.0.0"},
            {"protocol", "CryptoPegUSDT"}
        };
        res.set_content(j.dump(2), "application/json");
    });

    // 2. Estado General del Nodo y Bóveda
    server_->Get("/api/v1/node/status", [this](const httplib::Request&, httplib::Response& res) {
        const auto& vault = node_.get_vault();
        json j = {
            {"blockchain_height", node_.get_blockchain_height()},
            {"database_path", node_.get_db_path()},
            {"vault", {
                {"total_collateral_usdt", format_usdt(vault.get_total_collateral())},
                {"total_collateral_units", vault.get_total_collateral()},
                {"circulating_shielded_supply", format_usdt(vault.get_circulating_shielded_supply())},
                {"circulating_shielded_units", vault.get_circulating_shielded_supply()},
                {"fee_pool_reserve_usdt", format_usdt(vault.get_fee_pool_reserve())},
                {"fee_pool_reserve_units", vault.get_fee_pool_reserve()},
                {"deposit_fee_bps", vault.get_deposit_fee_bps()},
                {"withdraw_fee_bps", vault.get_withdraw_fee_bps()},
                {"is_solvent_1_to_1", node_.audit_system()}
            }},
            {"utxo_pool_count", node_.get_utxo_pool().size()}
        };
        res.set_content(j.dump(2), "application/json");
    });

    // 3. Explorador de Bloques Históricos
    server_->Get("/api/v1/chain/blocks", [this](const httplib::Request& req, httplib::Response& res) {
        uint64_t limit = 10;
        uint64_t offset = 0;
        if (req.has_param("limit")) {
            try { limit = std::stoull(req.get_param_value("limit")); } catch (...) {}
        }
        if (limit > 100) limit = 100; // Cota superior de seguridad contra DoS (V4-07)
        if (req.has_param("offset")) {
            try { offset = std::stoull(req.get_param_value("offset")); } catch (...) {}
        }

        uint64_t top = node_.get_blockchain_height();
        json blocks_arr = json::array();

        for (uint64_t h = offset; h <= top && (h - offset) < limit; ++h) {
            Block b;
            if (node_.get_block(h, b)) {
                blocks_arr.push_back(json{
                    {"height", b.header.height},
                    {"hash", to_hex(b.hash())},
                    {"prev_hash", to_hex(b.header.prev_block_hash)},
                    {"merkle_root", to_hex(b.header.merkle_root)},
                    {"timestamp", b.header.timestamp},
                    {"tx_count", b.txs.size()},
                    {"deposit_count", b.deposits.size()},
                    {"withdrawal_count", b.withdrawals.size()}
                });
            }
        }

        json j = {
            {"total_height", top},
            {"blocks", blocks_arr}
        };
        res.set_content(j.dump(2), "application/json");
    });

    // 4. Detalle de Bloque por Altura
    server_->Get(R"(/api/v1/chain/block/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        uint64_t height = 0;
        try {
            height = std::stoull(req.matches[1]);
        } catch (...) {
            res.status = 400;
            res.set_content(json{{"error", "Altura de bloque inválida"}}.dump(), "application/json");
            return;
        }

        Block b;
        if (!node_.get_block(height, b)) {
            res.status = 404;
            res.set_content(json{{"error", "Bloque no encontrado"}}.dump(), "application/json");
            return;
        }

        json txs_j = json::array();
        for (const auto& tx : b.txs) {
            txs_j.push_back(json{
                {"tx_hash", to_hex(tx.tx_hash)},
                {"key_image", to_hex(tx.ring_sig.key_image)},
                {"ring_size", tx.ring_sig.ring_pubkeys.size()},
                {"outputs_count", tx.outputs.size()},
                {"fee", tx.public_fee}
            });
        }

        json deps_j = json::array();
        for (const auto& dep : b.deposits) {
            deps_j.push_back(json{
                {"gross_deposited", format_usdt(dep.gross_usdt_deposited)},
                {"fee_to_pool", format_usdt(dep.fee_to_pool)},
                {"net_minted", format_usdt(dep.net_shielded_tokens_minted)},
                {"tx_hash", dep.tx_hash}
            });
        }

        json wdrs_j = json::array();
        for (const auto& w : b.withdrawals) {
            wdrs_j.push_back(json{
                {"gross_burned", format_usdt(w.gross_tokens_burned)},
                {"fee_to_pool", format_usdt(w.fee_to_pool)},
                {"fee_raw", w.fee_to_pool},
                {"net_tumbled", format_usdt(w.net_usdt_to_tumble)},
                {"net_amount_raw", w.net_usdt_to_tumble},
                {"order_id", w.order_id},
                {"destination", w.destination_address}
            });
        }

        json j = {
            {"height", b.header.height},
            {"hash", to_hex(b.hash())},
            {"prev_hash", to_hex(b.header.prev_block_hash)},
            {"merkle_root", to_hex(b.header.merkle_root)},
            {"timestamp", b.header.timestamp},
            {"transactions", txs_j},
            {"deposits", deps_j},
            {"withdrawals", wdrs_j}
        };
        res.set_content(j.dump(2), "application/json");
    });

    // 5. Generar Identidad Criptográfica (Billetera Stealth)
    server_->Post("/api/v1/wallet/generate", [](const httplib::Request&, httplib::Response& res) {
        auto wallet = StealthWallet::generate_random();
        json j = {
            {"stealth_address", wallet.get_public_address().encode()},
            {"spend_public_key", to_hex(wallet.spend_public_key)},
            {"view_public_key", to_hex(wallet.view_public_key)},
            {"spend_private_key", to_hex(wallet.spend_private_key)},
            {"view_private_key", to_hex(wallet.view_private_key)}
        };
        res.set_content(j.dump(2), "application/json");
    });

    // 6. Escaneo de Billetera (Soporta View-Key y Spend-Key opcional para filtrar gastados)
    server_->Post("/api/v1/wallet/scan", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string view_priv_hex = body.at("view_private_key").get<std::string>();
            std::string spend_pub_hex = body.at("spend_public_key").get<std::string>();

            StealthWallet scan_wallet;
            auto v_bytes = from_hex(view_priv_hex);
            auto s_bytes = from_hex(spend_pub_hex);
            if (v_bytes.size() != 32 || s_bytes.size() != 32) {
                throw std::invalid_argument("Las claves deben tener 32 bytes (64 caracteres hexadecimales).");
            }
            std::memcpy(scan_wallet.view_private_key.data(), v_bytes.data(), 32);
            std::memcpy(scan_wallet.spend_public_key.data(), s_bytes.data(), 32);
            sodium_memzero(v_bytes.data(), v_bytes.size());
            sodium_memzero(s_bytes.data(), s_bytes.size());

            bool has_spend_priv = false;
            if (body.contains("spend_private_key")) {
                std::string spend_priv_hex = body["spend_private_key"].get<std::string>();
                if (!spend_priv_hex.empty()) {
                    auto sp_bytes = from_hex(spend_priv_hex);
                    if (sp_bytes.size() == 32) {
                        std::memcpy(scan_wallet.spend_private_key.data(), sp_bytes.data(), 32);
                        has_spend_priv = true;
                    }
                    sodium_memzero(sp_bytes.data(), sp_bytes.size());
                }
            }

            Amount total_balance = 0;
            json outputs_j = json::array();
            json spent_outputs_j = json::array();

            for (const auto& u : node_.get_utxo_pool()) {
                if (StealthProtocol::scan_output(scan_wallet, u)) {
                    bool is_spent = false;
                    if (has_spend_priv) {
                        Key256 one_time_priv = StealthProtocol::derive_one_time_private_key(scan_wallet, u);
                        KeyImage img = RingSignatureEngine::compute_key_image(one_time_priv, u.destination_one_time);
                        secure_wipe(one_time_priv);
                        is_spent = node_.is_key_image_spent(img);
                    }

                    json item = {
                        {"destination_one_time", to_hex(u.destination_one_time)},
                        {"ephemeral_public_key", to_hex(u.ephemeral_public_key)},
                        {"amount_usdt", format_usdt(u.amount)},
                        {"amount_units", u.amount},
                        {"is_spent", is_spent}
                    };

                    if (is_spent) {
                        spent_outputs_j.push_back(item);
                    } else {
                        total_balance += u.amount;
                        outputs_j.push_back(item);
                    }
                }
            }

            // Limpieza de memoria (V4-11)
            if (has_spend_priv) {
                secure_wipe(scan_wallet.spend_private_key);
            }
            secure_wipe(scan_wallet.view_private_key);

            json j = {
                {"total_balance_usdt", format_usdt(total_balance)},
                {"total_balance_units", total_balance},
                {"outputs_count", outputs_j.size()},
                {"outputs", outputs_j},
                {"spent_outputs_count", spent_outputs_j.size()},
                {"spent_outputs", spent_outputs_j},
                {"is_watch_only", !has_spend_priv}
            };
            res.set_content(j.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // 7. Depósito de USDT Público -> Acuñación 1:1 en Bóveda
    server_->Post("/api/v1/vault/deposit", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string custom_tx = body.value("tx_hash", "");
            uint64_t custom_ts = body.value("timestamp", 0ULL);

            // 1. Rechazo categórico de depósitos simulados por CLI (Auditoría v3 - P0-01)
            if (custom_tx.rfind("0xCLI_", 0) == 0) {
                res.status = 400;
                res.set_content(json{{"error", "Acuñación simulada 0xCLI_ rechazada categóricamente (P0-01). Se requiere un depósito real confirmado en L2."}}.dump(), "application/json");
                return;
            }
            if (custom_tx.empty()) {
                res.status = 400;
                res.set_content(json{{"error", "El campo 'tx_hash' de Arbitrum es obligatorio para registrar un depósito."}}.dump(), "application/json");
                return;
            }

            // 2. Parseo seguro de monto (Auditoría v3 - P1-08)
            Amount gross = 0;
            if (body.contains("gross_usdt_raw")) {
                if (body.at("gross_usdt_raw").is_string()) {
                    gross = std::stoull(body.at("gross_usdt_raw").get<std::string>());
                } else {
                    gross = body.at("gross_usdt_raw").get<Amount>();
                }
            } else if (body.contains("gross_usdt")) {
                double gross_val = body.at("gross_usdt").get<double>();
                gross = parse_usdt(gross_val);
            } else {
                throw std::invalid_argument("Se requiere 'gross_usdt_raw' o 'gross_usdt'.");
            }

            // 3. Claves de destino Stealth DKSAP
            StealthAddress recipient;
            if (body.contains("recipient_stealth_address")) {
                recipient = StealthAddress::decode(body.at("recipient_stealth_address").get<std::string>());
            } else if (body.contains("stealth_pub_view") && body.contains("stealth_pub_spend")) {
                auto v_bytes = from_hex(body.at("stealth_pub_view").get<std::string>());
                auto s_bytes = from_hex(body.at("stealth_pub_spend").get<std::string>());
                if (v_bytes.size() != 32 || s_bytes.size() != 32) {
                    throw std::invalid_argument("Claves stealth de vista y gasto deben tener 32 bytes (64 caracteres hex).");
                }
                std::memcpy(recipient.view_public_key.data(), v_bytes.data(), 32);
                std::memcpy(recipient.spend_public_key.data(), s_bytes.data(), 32);
            } else {
                throw std::invalid_argument("Se requiere 'recipient_stealth_address' o ('stealth_pub_view' y 'stealth_pub_spend').");
            }

            // 4. Parámetros de Idempotencia Compuesta L2 (Auditoría v3 - P1-07)
            uint64_t chain_id = body.value("chain_id", 421614ULL);
            std::string contract_addr = body.value("contract_address", "");
            uint32_t log_index = body.value("log_index", 0U);
            uint64_t l2_block = body.value("l2_block_number", 0ULL);
            std::string event_id = body.value("event_id", "");
            if (event_id.empty()) {
                event_id = std::to_string(chain_id) + ":" + contract_addr + ":" + custom_tx + ":" + std::to_string(log_index);
            }

            if (node_.is_deposit_tx_processed(event_id) || node_.is_deposit_tx_processed(custom_tx)) {
                res.status = 409;
                res.set_content(json{{"error", "Transacción de depósito ya procesada previamente (idempotencia garantizada)."}}.dump(), "application/json");
                return;
            }

            // 5. Verificación de Certificado Criptográfico de Depósito firmado por Oráculo (P0-01)
            std::vector<std::pair<Key256, Signature64>> attested_signatures;

            Hash256 att_hash = compute_deposit_attestation_hash(
                chain_id, contract_addr, custom_tx, log_index, gross,
                recipient.view_public_key, recipient.spend_public_key, l2_block
            );

            // Soporte para array de firmas (M-de-N) o firma individual
            if (body.contains("validator_signatures") && body["validator_signatures"].is_array()) {
                for (const auto& item : body["validator_signatures"]) {
                    if (item.contains("pubkey") && item.contains("signature")) {
                        auto pk_bytes = from_hex(item["pubkey"].get<std::string>());
                        auto sig_bytes = from_hex(item["signature"].get<std::string>());
                        if (pk_bytes.size() == 32 && sig_bytes.size() == 64) {
                            Key256 v_pk;
                            Signature64 v_sig;
                            std::memcpy(v_pk.data(), pk_bytes.data(), 32);
                            std::memcpy(v_sig.data(), sig_bytes.data(), 64);
                            if (node_.is_authorized_validator(v_pk)) {
                                if (crypto_sign_verify_detached(v_sig.data(), att_hash.data(), 32, v_pk.data()) == 0) {
                                    bool exists = false;
                                    for (const auto& s : attested_signatures) {
                                        if (sodium_memcmp(s.first.data(), v_pk.data(), 32) == 0) {
                                            exists = true;
                                            break;
                                        }
                                    }
                                    if (!exists) {
                                        attested_signatures.push_back({v_pk, v_sig});
                                    }
                                }
                            }
                        }
                    }
                }
            } else if (body.contains("validator_pubkey") && body.contains("validator_signature")) {
                auto pk_bytes = from_hex(body.at("validator_pubkey").get<std::string>());
                auto sig_bytes = from_hex(body.at("validator_signature").get<std::string>());
                if (pk_bytes.size() == 32 && sig_bytes.size() == 64) {
                    Key256 v_pk;
                    Signature64 v_sig;
                    std::memcpy(v_pk.data(), pk_bytes.data(), 32);
                    std::memcpy(v_sig.data(), sig_bytes.data(), 64);
                    if (node_.is_authorized_validator(v_pk)) {
                        if (crypto_sign_verify_detached(v_sig.data(), att_hash.data(), 32, v_pk.data()) == 0) {
                            attested_signatures.push_back({v_pk, v_sig});
                        }
                    }
                }
            }

            if (attested_signatures.empty()) {
                res.status = 401;
                res.set_content(json{{"error", "No autorizado: se requiere un certificado criptográfico de depósito válido firmado por un oráculo autorizado (P0-01)."}}.dump(), "application/json");
                return;
            }

            auto receipt = node_.buy_shielded(gross, recipient, event_id, custom_ts);

            json j = {
                {"success", true},
                {"block_height", node_.get_blockchain_height()},
                {"gross_usdt", format_usdt(receipt.gross_usdt_deposited)},
                {"fee_to_pool", format_usdt(receipt.fee_to_pool)},
                {"net_shielded_minted", format_usdt(receipt.net_shielded_tokens_minted)},
                {"tx_hash", receipt.tx_hash}
            };
            res.set_content(j.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // 8. Transferencia Confidencial (DKSAP + RingCT + Señuelos)
    server_->Post("/api/v1/tx/transfer", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string spend_priv_hex = body.at("sender_spend_private_key").get<std::string>();
            std::string view_priv_hex = body.at("sender_view_private_key").get<std::string>();
            std::string input_pub_hex = body.at("input_utxo_pubkey").get<std::string>();
            std::string recipient_str = body.at("recipient_stealth_address").get<std::string>();
            double send_val = body.at("amount_usdt").get<double>();
            double fee_val = body.value("tx_fee_usdt", 0.0);

            Amount send_amount = parse_usdt(send_val);
            Amount tx_fee = parse_usdt(fee_val);
            StealthAddress recipient = StealthAddress::decode(recipient_str);

            StealthWallet sender;
            auto sp_bytes = from_hex(spend_priv_hex);
            auto vp_bytes = from_hex(view_priv_hex);
            if (sp_bytes.size() != 32 || vp_bytes.size() != 32) {
                sodium_memzero(sp_bytes.data(), sp_bytes.size());
                sodium_memzero(vp_bytes.data(), vp_bytes.size());
                throw std::invalid_argument("Claves de emisor deben tener 32 bytes.");
            }
            std::memcpy(sender.spend_private_key.data(), sp_bytes.data(), 32);
            std::memcpy(sender.view_private_key.data(), vp_bytes.data(), 32);
            sodium_memzero(sp_bytes.data(), sp_bytes.size());
            sodium_memzero(vp_bytes.data(), vp_bytes.size());
            crypto_scalarmult_ed25519_base_noclamp(sender.spend_public_key.data(), sender.spend_private_key.data());
            crypto_scalarmult_ed25519_base_noclamp(sender.view_public_key.data(), sender.view_private_key.data());

            // Localizar el UTXO de entrada por clave pública de destino
            auto target_pub = from_hex(input_pub_hex);
            OneTimeOutput selected_utxo;
            bool found_utxo = false;
            for (const auto& u : node_.get_utxo_pool()) {
                if (sodium_memcmp(u.destination_one_time.data(), target_pub.data(), 32) == 0) {
                    selected_utxo = u;
                    found_utxo = true;
                    break;
                }
            }
            if (!found_utxo) {
                secure_wipe(sender.spend_private_key);
                secure_wipe(sender.view_private_key);
                throw std::runtime_error("No se encontró el UTXO de entrada especificado.");
            }

            auto tx = node_.transfer_shielded(sender, selected_utxo, recipient, send_amount, tx_fee);
            secure_wipe(sender.spend_private_key);
            secure_wipe(sender.view_private_key);

            json j = {
                {"success", true},
                {"block_height", node_.get_blockchain_height()},
                {"tx_hash", to_hex(tx.tx_hash)},
                {"key_image", to_hex(tx.ring_sig.key_image)},
                {"ring_size", tx.ring_sig.ring_pubkeys.size()},
                {"sent_amount", format_usdt(send_amount)},
                {"fee", format_usdt(tx_fee)}
            };
            res.set_content(j.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // 8.1. Dispensador de Señuelos para Firma No-Custodial en Cliente Wasm
    server_->Get("/api/v1/chain/decoys", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            size_t count = 4;
            if (req.has_param("count")) {
                count = static_cast<size_t>(std::stoul(req.get_param_value("count")));
                if (count > 64) count = 64; // Cota de cordura
            }
            Key256 exclude_pub{};
            if (req.has_param("exclude")) {
                auto ex_bytes = from_hex(req.get_param_value("exclude"));
                if (ex_bytes.size() == 32) {
                    std::memcpy(exclude_pub.data(), ex_bytes.data(), 32);
                }
            }

            Amount target_amount = 0;
            if (req.has_param("amount")) {
                target_amount = static_cast<Amount>(std::stoull(req.get_param_value("amount")));
            }

            auto decoys = node_.get_random_decoys(count, exclude_pub, target_amount);
            json decoys_j = json::array();
            for (const auto& d : decoys) {
                decoys_j.push_back(to_hex(d));
            }
            json j = {
                {"count", decoys.size()},
                {"decoys", decoys_j}
            };
            res.set_content(j.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // 8.2. Envío de Transacción Confidencial Pre-firmada por Cliente Wasm (No-Custodial Fase 3)
    server_->Post("/api/v1/tx/push", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            // Salvaguarda no-custodial: validación estricta en submit_pre_signed_transaction
            const char* dis_flag = std::getenv("DISABLE_TX_PUSH");
            if (dis_flag != nullptr && (std::string(dis_flag) == "true" || std::string(dis_flag) == "1")) {
                res.status = 403;
                res.set_content(json{{"error", "Endpoint /api/v1/tx/push deshabilitado administrativamente."}}.dump(2), "application/json");
                return;
            }

            auto body = json::parse(req.body);
            ShieldedTransaction tx;

            std::string tx_hash_hex = body.at("tx_hash").get<std::string>();
            auto th_bytes = from_hex(tx_hash_hex);
            if (th_bytes.size() != 32) throw std::invalid_argument("tx_hash debe tener 32 bytes.");
            std::memcpy(tx.tx_hash.data(), th_bytes.data(), 32);

            double fee_val = body.value("fee_usdt", 0.0);
            tx.public_fee = parse_usdt(fee_val);
            tx.timestamp = body.value("timestamp", "2026-09-13 00:00:00 UTC");

            // Deserializar salidas (outputs)
            const auto& outputs_arr = body.at("outputs");
            for (const auto& out_j : outputs_arr) {
                OneTimeOutput out;
                auto eph_bytes = from_hex(out_j.at("ephemeral_pubkey").get<std::string>());
                auto dst_bytes = from_hex(out_j.at("destination_one_time").get<std::string>());
                if (eph_bytes.size() != 32 || dst_bytes.size() != 32) {
                    throw std::invalid_argument("Claves de salida deben tener 32 bytes.");
                }
                std::memcpy(out.ephemeral_public_key.data(), eph_bytes.data(), 32);
                std::memcpy(out.destination_one_time.data(), dst_bytes.data(), 32);
                out.amount = parse_usdt(out_j.at("amount_usdt").get<double>());
                tx.outputs.push_back(out);
            }

            // Deserializar firma de anillo (ring_sig)
            const auto& sig_j = body.at("ring_sig");
            auto ki_bytes = from_hex(sig_j.at("key_image").get<std::string>());
            auto c0_bytes = from_hex(sig_j.at("c0").get<std::string>());
            if (ki_bytes.size() != 32 || c0_bytes.size() != 32) {
                throw std::invalid_argument("key_image y c0 deben tener 32 bytes.");
            }
            std::memcpy(tx.ring_sig.key_image.data(), ki_bytes.data(), 32);
            std::memcpy(tx.ring_sig.c0.data(), c0_bytes.data(), 32);

            const auto& ring_arr = sig_j.at("ring_pubkeys");
            for (const auto& r_hex : ring_arr) {
                auto pk_bytes = from_hex(r_hex.get<std::string>());
                if (pk_bytes.size() != 32) throw std::invalid_argument("Miembro del anillo debe tener 32 bytes.");
                Key256 pk;
                std::memcpy(pk.data(), pk_bytes.data(), 32);
                tx.ring_sig.ring_pubkeys.push_back(pk);
            }

            const auto& resp_arr = sig_j.at("responses");
            for (const auto& resp_hex : resp_arr) {
                auto s_bytes = from_hex(resp_hex.get<std::string>());
                if (s_bytes.size() != 32) throw std::invalid_argument("Respuesta de firma debe tener 32 bytes.");
                Key256 s;
                std::memcpy(s.data(), s_bytes.data(), 32);
                tx.ring_sig.responses.push_back(s);
            }

            // Asimilar la transacción pre-firmada a través del nodo
            auto accepted_tx = node_.submit_pre_signed_transaction(tx);

            json j = {
                {"success", true},
                {"block_height", node_.get_blockchain_height()},
                {"tx_hash", to_hex(accepted_tx.tx_hash)},
                {"key_image", to_hex(accepted_tx.ring_sig.key_image)},
                {"ring_size", accepted_tx.ring_sig.ring_pubkeys.size()},
                {"fee", format_usdt(accepted_tx.public_fee)}
            };
            res.set_content(j.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // 9. Solicitud de Retiro -> Quema de Tokens y Dispersión Tumbler
    server_->Post("/api/v1/vault/withdraw", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string spend_priv_hex = body.at("burner_spend_private_key").get<std::string>();
            std::string view_priv_hex = body.at("burner_view_private_key").get<std::string>();
            std::string input_pub_hex = body.at("input_utxo_pubkey").get<std::string>();
            double withdraw_val = body.at("tokens_to_withdraw").get<double>();
            std::string destination_wallet = body.at("destination_public_usdt").get<std::string>();
            if (!is_valid_evm_address(destination_wallet)) {
                res.status = 400;
                res.set_content(json{{"error", "Direccion de destino invalida: debe ser una direccion EVM valida (0x + 40 caracteres hex, distinta de cero)."}}.dump(2), "application/json");
                return;
            }

            Amount withdraw_amount = parse_usdt(withdraw_val);

            StealthWallet burner;
            auto sp_bytes = from_hex(spend_priv_hex);
            auto vp_bytes = from_hex(view_priv_hex);
            if (sp_bytes.size() != 32 || vp_bytes.size() != 32) {
                sodium_memzero(sp_bytes.data(), sp_bytes.size());
                sodium_memzero(vp_bytes.data(), vp_bytes.size());
                throw std::invalid_argument("Claves de emisor deben tener 32 bytes.");
            }
            std::memcpy(burner.spend_private_key.data(), sp_bytes.data(), 32);
            std::memcpy(burner.view_private_key.data(), vp_bytes.data(), 32);
            sodium_memzero(sp_bytes.data(), sp_bytes.size());
            sodium_memzero(vp_bytes.data(), vp_bytes.size());
            crypto_scalarmult_ed25519_base_noclamp(burner.spend_public_key.data(), burner.spend_private_key.data());
            crypto_scalarmult_ed25519_base_noclamp(burner.view_public_key.data(), burner.view_private_key.data());

            auto target_pub = from_hex(input_pub_hex);
            OneTimeOutput selected_utxo;
            bool found_utxo = false;
            for (const auto& u : node_.get_utxo_pool()) {
                if (sodium_memcmp(u.destination_one_time.data(), target_pub.data(), 32) == 0) {
                    selected_utxo = u;
                    found_utxo = true;
                    break;
                }
            }
            if (!found_utxo) {
                secure_wipe(burner.spend_private_key);
                secure_wipe(burner.view_private_key);
                throw std::runtime_error("No se encontró el UTXO de retiro especificado.");
            }

            auto plan = node_.withdraw_shielded(burner, selected_utxo, withdraw_amount, destination_wallet, true);
            secure_wipe(burner.spend_private_key);
            secure_wipe(burner.view_private_key);

            json routes_j = json::array();
            for (const auto& r : plan.routes) {
                routes_j.push_back(json{
                    {"fragment_index", r.fragment_index},
                    {"net_amount_usdt", format_usdt(r.fragment_amount)},
                    {"hops_count", r.hops.size()}
                });
            }

            json j = {
                {"success", true},
                {"block_height", node_.get_blockchain_height()},
                {"order_id", plan.order_id},
                {"gross_tokens_burned", format_usdt(plan.total_gross_tokens)},
                {"fee_to_pool", format_usdt(plan.fee_deducted_to_pool)},
                {"net_usdt_tumbled", format_usdt(plan.total_net_usdt)},
                {"destination", plan.destination_address},
                {"total_micro_fragments", plan.total_fragments},
                {"routes", routes_j}
            };
            res.set_content(j.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // 10. Cobro de Comisiones de Tesorería (Administración del Protocolo V4-02)
    server_->Post("/api/v1/vault/claim-fees", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            const char* env_admin_token = std::getenv("ADMIN_AUTH_TOKEN");
            std::string expected_admin = env_admin_token ? env_admin_token : "";
            std::string auth_header = req.get_header_value("X-Admin-Token");
            if (auth_header.empty()) {
                auth_header = req.get_header_value("Authorization");
                if (auth_header.rfind("Bearer ", 0) == 0) {
                    auth_header = auth_header.substr(7);
                }
            }
            if (expected_admin.empty() || auth_header.size() != expected_admin.size() ||
                sodium_memcmp(auth_header.data(), expected_admin.data(), expected_admin.size()) != 0) {
                res.status = 401;
                res.set_content(json{{"error", "No autorizado: se requiere un token administrativo valido en X-Admin-Token o Authorization."}}.dump(), "application/json");
                return;
            }

            auto body = json::parse(req.body);
            double amount_val = body.at("amount_usdt").get<double>();
            std::string treasury_addr = body.at("treasury_address").get<std::string>();
            if (!is_valid_evm_address(treasury_addr)) {
                res.status = 400;
                res.set_content(json{{"error", "Direccion de tesoreria invalida: debe ser una direccion EVM valida (0x + 40 caracteres hex, distinta de cero)."}}.dump(2), "application/json");
                return;
            }

            Amount claim_amount = parse_usdt(amount_val);
            auto receipt = node_.claim_treasury_fees(claim_amount, treasury_addr);

            json j = {
                {"success", true},
                {"amount_claimed_usdt", format_usdt(receipt.amount_claimed)},
                {"remaining_fee_pool_usdt", format_usdt(receipt.remaining_fee_pool)},
                {"destination_address", receipt.destination_address},
                {"tx_hash", receipt.tx_hash},
                {"is_solvent_1_to_1", node_.audit_system()}
            };
            res.set_content(j.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // =========================================================================
    // ENDPOINTS DE RED P2P (GOSSIP, HANDSHAKE, PEERS, SYNC)
    // =========================================================================

    // 10. Estado P2P
    server_->Get("/api/v1/p2p/status", [this](const httplib::Request&, httplib::Response& res) {
        json peers_j = json::array();
        if (p2p_manager_) {
            for (const auto& p : p2p_manager_->get_active_peers()) {
                peers_j.push_back(json{
                    {"node_id", p.node_id},
                    {"address", p.address},
                    {"height", p.height},
                    {"top_hash", to_hex(p.top_hash)},
                    {"last_seen", p.last_seen_timestamp},
                    {"latency_ms", p.latency_ms},
                    {"is_connected", p.is_connected}
                });
            }
        }
        json j = {
            {"p2p_enabled", p2p_manager_ != nullptr},
            {"node_id", p2p_manager_ ? p2p_manager_->get_node_id() : ""},
            {"listen_url", p2p_manager_ ? p2p_manager_->get_local_listen_url() : ""},
            {"peer_count", p2p_manager_ ? p2p_manager_->get_peer_count() : 0},
            {"peers", peers_j}
        };
        res.set_content(j.dump(2), "application/json");
    });

    // 11. Lista de Pares
    server_->Get("/api/v1/p2p/peers", [this](const httplib::Request&, httplib::Response& res) {
        json j = {
            {"peers", p2p_manager_ ? p2p_manager_->get_peer_urls() : std::vector<std::string>{}}
        };
        res.set_content(j.dump(2), "application/json");
    });

    // 12. Agregar Par Manualmente
    server_->Post("/api/v1/p2p/peers", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string peer_url = body.at("peer_url").get<std::string>();
            bool ok = p2p_manager_ ? p2p_manager_->add_peer(peer_url) : false;
            res.set_content(json{{"success", ok}, {"peer_url", peer_url}}.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // 13. Handshake P2P Mutuo
    server_->Post("/api/v1/p2p/handshake", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string remote_id = body.value("node_id", "");
            std::string remote_listen_url = body.value("listen_url", "");
            uint64_t remote_height = body.value("height", 0ULL);
            std::string remote_top_hash_hex = body.value("top_hash", "");

            Hash256 r_top_hash{};
            if (!remote_top_hash_hex.empty()) {
                auto b = from_hex(remote_top_hash_hex);
                if (b.size() == 32) std::memcpy(r_top_hash.data(), b.data(), 32);
            }

            if (p2p_manager_ && !remote_listen_url.empty()) {
                p2p_manager_->register_incoming_peer(remote_listen_url, remote_id, remote_height, r_top_hash);
            }

            json response = {
                {"node_id", p2p_manager_ ? p2p_manager_->get_node_id() : "node-local"},
                {"height", node_.get_blockchain_height()},
                {"top_hash", to_hex(node_.get_top_block_hash())},
                {"known_peers", p2p_manager_ ? p2p_manager_->get_peer_urls() : std::vector<std::string>{}}
            };
            res.set_content(response.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // 14. Difusión Gossip de Bloques (Recepción e Intercambio)
    server_->Post("/api/v1/p2p/block", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string hex_str = body.at("block_hex").get<std::string>();
            std::string sender_url = body.value("sender_listen_url", "");

            auto bytes = from_hex(hex_str);
            Block blk = Block::deserialize(bytes.data(), bytes.size());

            // Solo ignorar si el bloque es estrictamente anterior a la punta local (V4-01)
            // Si blk.header.height == current_height, permitir que apply_remote_block evalúe Fork-Choice por quórum
            if (blk.header.height < node_.get_blockchain_height()) {
                res.set_content(json{{"status", "ignored"}, {"reason", "older_than_tip"}}.dump(), "application/json");
                return;
            }

            std::string err;
            if (!node_.apply_remote_block(blk, err)) {
                if (blk.header.height > node_.get_blockchain_height() + 1 && p2p_manager_ && !sender_url.empty()) {
                    p2p_manager_->sync_from_peer(sender_url);
                }
                res.status = 400;
                res.set_content(json{{"error", err}}.dump(), "application/json");
                return;
            }

            if (p2p_manager_) {
                p2p_manager_->broadcast_block(blk, sender_url);
            }

            res.set_content(json{{"status", "accepted"}, {"height", blk.header.height}}.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // 15. Sincronización de Cadena (Initial Block Download - IBD)
    server_->Get("/api/v1/p2p/sync", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            uint64_t from_h = 1;
            if (req.has_param("from_height")) {
                from_h = std::stoull(req.get_param_value("from_height"));
            }
            size_t limit = 50;
            if (req.has_param("limit")) {
                limit = std::stoul(req.get_param_value("limit"));
            }
            if (limit > 100) limit = 100; // Cota de seguridad contra DoS (V4-07)

            uint64_t top_h = node_.get_blockchain_height();
            json blocks_j = json::array();

            for (uint64_t h = from_h; h <= top_h && blocks_j.size() < limit; ++h) {
                Block b;
                if (node_.get_block(h, b)) {
                    auto raw = b.serialize();
                    blocks_j.push_back(json{
                        {"height", b.header.height},
                        {"hash", to_hex(b.hash())},
                        {"block_hex", to_hex(raw.data(), raw.size())}
                    });
                }
            }

            json response = {
                {"from_height", from_h},
                {"top_height", top_h},
                {"count", blocks_j.size()},
                {"blocks", blocks_j}
            };
            res.set_content(response.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace crypto
