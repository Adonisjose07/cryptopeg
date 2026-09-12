#include "wallet_manager.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <cstring>

using json = nlohmann::json;

namespace crypto {

WalletManager::WalletManager(const std::string& daemon_host, int daemon_port)
    : daemon_host_(daemon_host), daemon_port_(daemon_port) {
    daemon_url_ = "http://" + daemon_host_ + ":" + std::to_string(daemon_port_);
}

WalletManager::~WalletManager() = default;

void WalletManager::set_daemon(const std::string& host, int port) {
    daemon_host_ = host;
    daemon_port_ = port;
    daemon_url_ = "http://" + daemon_host_ + ":" + std::to_string(daemon_port_);
}

std::string WalletManager::create_wallet(const std::string& name) {
    name_ = name;
    mnemonic_ = MnemonicEngine::generate_24_words();
    wallet_ = std::make_unique<StealthWallet>(MnemonicEngine::mnemonic_to_wallet(mnemonic_));
    is_loaded_ = true;
    balance_ = 0;
    owned_utxos_.clear();
    return mnemonic_;
}

void WalletManager::restore_wallet(const std::string& name, const std::string& mnemonic) {
    if (!MnemonicEngine::validate_mnemonic(mnemonic)) {
        throw std::invalid_argument("Frase semilla mnemónica inválida o checksum incorrecto.");
    }
    name_ = name;
    mnemonic_ = mnemonic;
    wallet_ = std::make_unique<StealthWallet>(MnemonicEngine::mnemonic_to_wallet(mnemonic));
    is_loaded_ = true;
    balance_ = 0;
    owned_utxos_.clear();
}

StealthAddress WalletManager::get_address() const {
    if (!is_loaded_ || !wallet_) {
        throw std::runtime_error("No hay ninguna billetera cargada.");
    }
    return wallet_->get_public_address();
}

void WalletManager::save_wallet(const std::string& filepath) {
    if (!is_loaded_ || !wallet_) {
        throw std::runtime_error("No hay ninguna billetera activa para guardar.");
    }
    json j = {
        {"name", name_},
        {"mnemonic", mnemonic_},
        {"stealth_address", wallet_->get_public_address().encode()},
        {"spend_public_key", to_hex(wallet_->spend_public_key)},
        {"view_public_key", to_hex(wallet_->view_public_key)}
    };
    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
        throw std::runtime_error("No se pudo abrir el archivo para guardar: " + filepath);
    }
    ofs << j.dump(2);
}

void WalletManager::load_wallet(const std::string& filepath) {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        throw std::runtime_error("No se pudo abrir el archivo de billetera: " + filepath);
    }
    json j = json::parse(ifs);
    std::string name = j.at("name");
    std::string mnemonic = j.at("mnemonic");
    restore_wallet(name, mnemonic);
}

bool WalletManager::sync(std::string& error_msg) {
    if (!is_loaded_ || !wallet_) {
        error_msg = "No hay ninguna billetera activa para sincronizar.";
        return false;
    }

    try {
        httplib::Client cli(daemon_url_.c_str());
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(10, 0);

        json scan_req = {
            {"view_private_key", to_hex(wallet_->view_private_key)},
            {"spend_public_key", to_hex(wallet_->spend_public_key)},
            {"spend_private_key", to_hex(wallet_->spend_private_key)}
        };

        auto res = cli.Post("/api/v1/wallet/scan", scan_req.dump(), "application/json");
        if (!res) {
            error_msg = "No se pudo conectar con el nodo en " + daemon_url_;
            return false;
        }
        if (res->status != 200) {
            auto err_json = json::parse(res->body, nullptr, false);
            error_msg = (err_json.is_object() && err_json.contains("error"))
                ? err_json["error"].get<std::string>()
                : "Error HTTP " + std::to_string(res->status);
            return false;
        }

        auto data = json::parse(res->body);
        balance_ = data["total_balance_units"].get<Amount>();
        owned_utxos_.clear();

        for (const auto& o : data["outputs"]) {
            OneTimeOutput utxo;
            auto dest_bytes = from_hex(o["destination_one_time"].get<std::string>());
            auto eph_bytes = from_hex(o["ephemeral_public_key"].get<std::string>());
            std::memcpy(utxo.destination_one_time.data(), dest_bytes.data(), 32);
            std::memcpy(utxo.ephemeral_public_key.data(), eph_bytes.data(), 32);
            utxo.amount = o["amount_units"].get<Amount>();
            owned_utxos_.push_back(utxo);
        }

        return true;
    } catch (const std::exception& e) {
        error_msg = e.what();
        return false;
    }
}

bool WalletManager::deposit(Amount gross_usdt, std::string& out_tx_hash, std::string& error_msg) {
    if (!is_loaded_ || !wallet_) {
        error_msg = "No hay ninguna billetera activa.";
        return false;
    }

    try {
        httplib::Client cli(daemon_url_.c_str());
        double gross_val = static_cast<double>(gross_usdt) / 1000000.0;

        // Generar identificador único de tx para idempotencia
        Key256 rand_tx;
        randombytes_buf(rand_tx.data(), rand_tx.size());
        std::string mock_tx = "0xCLI_" + to_hex(rand_tx);

        json dep_req = {
            {"gross_usdt", gross_val},
            {"recipient_stealth_address", wallet_->get_public_address().encode()},
            {"tx_hash", mock_tx}
        };

        const char* env_secret = std::getenv("ORACLE_SECRET");
        std::string secret = env_secret ? env_secret : "cryptopeg_oracle_secret_2026";
        httplib::Headers headers = {
            {"X-Oracle-Secret", secret}
        };

        auto res = cli.Post("/api/v1/vault/deposit", headers, dep_req.dump(), "application/json");
        if (!res || res->status != 200) {
            error_msg = res ? res->body : "Fallo de conexión";
            return false;
        }

        auto data = json::parse(res->body);
        out_tx_hash = data["tx_hash"].get<std::string>();

        // Sincronizar automáticamente tras depósito
        std::string sync_err;
        sync(sync_err);

        return true;
    } catch (const std::exception& e) {
        error_msg = e.what();
        return false;
    }
}

bool WalletManager::transfer(
    const std::string& recipient_address,
    Amount amount,
    std::string& out_tx_hash,
    std::string& error_msg
) {
    if (!is_loaded_ || !wallet_) {
        error_msg = "No hay ninguna billetera activa.";
        return false;
    }

    // Asegurar sincronización previa
    std::string sync_err;
    sync(sync_err);

    if (owned_utxos_.empty()) {
        error_msg = "No tienes ningún UTXO con fondos disponibles.";
        return false;
    }

    // Seleccionar un UTXO adecuado
    const OneTimeOutput* selected = nullptr;
    for (const auto& u : owned_utxos_) {
        if (u.amount >= amount) {
            selected = &u;
            break;
        }
    }

    if (!selected) {
        error_msg = "No tienes ningún UTXO individual con suficiente saldo para cubrir " + format_usdt(amount);
        return false;
    }

    try {
        httplib::Client cli(daemon_url_.c_str());
        double amount_val = static_cast<double>(amount) / 1000000.0;

        json tx_req = {
            {"sender_spend_private_key", to_hex(wallet_->spend_private_key)},
            {"sender_view_private_key", to_hex(wallet_->view_private_key)},
            {"input_utxo_pubkey", to_hex(selected->destination_one_time)},
            {"recipient_stealth_address", recipient_address},
            {"amount_usdt", amount_val},
            {"tx_fee_usdt", 0.0}
        };

        auto res = cli.Post("/api/v1/tx/transfer", tx_req.dump(), "application/json");
        if (!res || res->status != 200) {
            auto err_j = json::parse(res ? res->body : "{}", nullptr, false);
            error_msg = (err_j.is_object() && err_j.contains("error"))
                ? err_j["error"].get<std::string>()
                : (res ? res->body : "Fallo de conexión");
            return false;
        }

        auto data = json::parse(res->body);
        out_tx_hash = data["tx_hash"].get<std::string>();

        // Re-sincronizar
        sync(sync_err);
        return true;
    } catch (const std::exception& e) {
        error_msg = e.what();
        return false;
    }
}

bool WalletManager::withdraw(
    Amount amount,
    const std::string& destination_public_usdt,
    std::string& out_order_id,
    size_t& out_fragments_count,
    std::string& error_msg
) {
    if (!is_loaded_ || !wallet_) {
        error_msg = "No hay ninguna billetera activa.";
        return false;
    }

    std::string sync_err;
    sync(sync_err);

    if (owned_utxos_.empty()) {
        error_msg = "No tienes ningún UTXO con fondos para retirar.";
        return false;
    }

    const OneTimeOutput* selected = nullptr;
    for (const auto& u : owned_utxos_) {
        if (u.amount >= amount) {
            selected = &u;
            break;
        }
    }

    if (!selected) {
        error_msg = "Fondos insuficientes en tus UTXOs individuales para retirar " + format_usdt(amount);
        return false;
    }

    try {
        httplib::Client cli(daemon_url_.c_str());
        double amount_val = static_cast<double>(amount) / 1000000.0;

        json wdr_req = {
            {"burner_spend_private_key", to_hex(wallet_->spend_private_key)},
            {"burner_view_private_key", to_hex(wallet_->view_private_key)},
            {"input_utxo_pubkey", to_hex(selected->destination_one_time)},
            {"tokens_to_withdraw", amount_val},
            {"destination_public_usdt", destination_public_usdt}
        };

        auto res = cli.Post("/api/v1/vault/withdraw", wdr_req.dump(), "application/json");
        if (!res || res->status != 200) {
            auto err_j = json::parse(res ? res->body : "{}", nullptr, false);
            error_msg = (err_j.is_object() && err_j.contains("error"))
                ? err_j["error"].get<std::string>()
                : (res ? res->body : "Fallo de conexión");
            return false;
        }

        auto data = json::parse(res->body);
        out_order_id = data["order_id"].get<std::string>();
        out_fragments_count = data["total_micro_fragments"].get<size_t>();

        sync(sync_err);
        return true;
    } catch (const std::exception& e) {
        error_msg = e.what();
        return false;
    }
}

void WalletManager::close_wallet() {
    is_loaded_ = false;
    name_.clear();
    mnemonic_.clear();
    wallet_.reset();
    balance_ = 0;
    owned_utxos_.clear();
}

bool WalletManager::get_node_status(std::string& raw_json, std::string& error_msg) {
    try {
        httplib::Client cli(daemon_url_.c_str());
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(5, 0);
        auto res = cli.Get("/api/v1/node/status");
        if (!res || res->status != 200) {
            error_msg = res ? "HTTP " + std::to_string(res->status) : "No se pudo conectar al nodo en " + daemon_url_;
            return false;
        }
        raw_json = res->body;
        return true;
    } catch (const std::exception& e) {
        error_msg = e.what();
        return false;
    }
}

} // namespace crypto
