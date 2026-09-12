#pragma once

#include "types.hpp"
#include "stealth.hpp"
#include "mnemonic.hpp"
#include <string>
#include <vector>
#include <memory>

namespace crypto {

class WalletManager {
public:
    explicit WalletManager(const std::string& daemon_host = "127.0.0.1", int daemon_port = 8080);
    ~WalletManager();

    // Crear una nueva billetera generando una frase semilla de 24 palabras
    std::string create_wallet(const std::string& name);

    // Restaurar billetera existente a partir de las 24 palabras mnemónicas
    void restore_wallet(const std::string& name, const std::string& mnemonic);

    // Guardar / Cargar billetera en archivo local (.wallet)
    void save_wallet(const std::string& filepath);
    void load_wallet(const std::string& filepath);

    // Sincronizar saldo con el nodo daemon remoto utilizando escaneo por View-Key
    bool sync(std::string& error_msg);

    // Comprar USDT colateralizado (acuñación 1:1) a la propia dirección stealth
    bool deposit(Amount gross_usdt, std::string& out_tx_hash, std::string& error_msg);

    // Enviar transferencia privada a otra dirección stealth usando firmas de anillo
    bool transfer(
        const std::string& recipient_address,
        Amount amount,
        std::string& out_tx_hash,
        std::string& error_msg
    );

    // Solicitar retiro 1:1 con mezclador (micro-tumbler) hacia una dirección pública
    bool withdraw(
        Amount amount,
        const std::string& destination_public_usdt,
        std::string& out_order_id,
        size_t& out_fragments_count,
        std::string& error_msg
    );

    // Cerrar sesión / Descargar billetera activa
    void close_wallet();

    // Consultar estado general del nodo daemon
    bool get_node_status(std::string& raw_json, std::string& error_msg);

    // Consultas
    bool has_wallet() const { return is_loaded_; }
    const std::string& get_name() const { return name_; }
    const std::string& get_mnemonic() const { return mnemonic_; }
    StealthAddress get_address() const;
    Amount get_balance() const { return balance_; }
    const std::vector<OneTimeOutput>& get_utxos() const { return owned_utxos_; }
    const std::string& get_daemon_url() const { return daemon_url_; }
    void set_daemon(const std::string& host, int port);

private:
    std::string daemon_host_;
    int daemon_port_;
    std::string daemon_url_;

    bool is_loaded_{false};
    std::string name_;
    std::string mnemonic_;
    std::unique_ptr<StealthWallet> wallet_;
    Amount balance_{0};
    std::vector<OneTimeOutput> owned_utxos_;
};

} // namespace crypto
