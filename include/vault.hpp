#pragma once

#include "types.hpp"
#include <mutex>
#include <string>

namespace crypto {

struct DepositReceipt {
    Amount gross_usdt_deposited{0};
    Amount fee_to_pool{0};
    Amount net_shielded_tokens_minted{0};
    std::string tx_hash;
    Key256 recipient_view_pub{};
    Key256 recipient_spend_pub{};
};

struct WithdrawalReceipt {
    Amount gross_tokens_burned{0};
    Amount fee_to_pool{0};
    Amount net_usdt_to_tumble{0};
    std::string order_id;
    std::string destination_address;
    KeyImage key_image{};
    Key256 burned_utxo_pubkey{};
    Key256 burn_signature_c0{};
    Key256 burn_signature_s{};
    Key256 change_output_pubkey{};
};

struct ClaimReceipt {
    Amount amount_claimed;
    Amount remaining_fee_pool;
    std::string destination_address;
    std::string tx_hash;
};

class Vault {
public:
    explicit Vault(uint32_t deposit_fee_bps = 50, uint32_t withdraw_fee_bps = 50);

    // Constructor de copia explícito (std::mutex no es copiable) (AUD-CP-01)
    Vault(const Vault& other) {
        std::lock_guard<std::mutex> lock(other.mutex_);
        total_collateral_ = other.total_collateral_;
        circulating_supply_ = other.circulating_supply_;
        fee_pool_reserve_ = other.fee_pool_reserve_;
        deposit_fee_bps_ = other.deposit_fee_bps_;
        withdraw_fee_bps_ = other.withdraw_fee_bps_;
    }

    Vault& operator=(const Vault& other) {
        if (this != &other) {
            std::unique_lock<std::mutex> lock1(mutex_, std::defer_lock);
            std::unique_lock<std::mutex> lock2(other.mutex_, std::defer_lock);
            std::lock(lock1, lock2);
            total_collateral_ = other.total_collateral_;
            circulating_supply_ = other.circulating_supply_;
            fee_pool_reserve_ = other.fee_pool_reserve_;
            deposit_fee_bps_ = other.deposit_fee_bps_;
            withdraw_fee_bps_ = other.withdraw_fee_bps_;
        }
        return *this;
    }

    // Depósito de USDT público -> Deducción comisión -> Acuñación de tokens privados 1:1
    DepositReceipt deposit(Amount usdt_gross, const std::string& custom_tx_hash = "");

    // Solicitud de Retiro -> Quema de tokens privados -> Deducción comisión -> USDT neto para el mezclador
    WithdrawalReceipt request_withdrawal(Amount tokens_gross);

    // Cobro de comisiones acumuladas para la tesorería (sin tocar el colateral circulante de los usuarios)
    ClaimReceipt claim_fees(Amount amount_to_claim, const std::string& destination_address);

    // Consulta de balances (en micro-USDT)
    Amount get_total_collateral() const;
    Amount get_circulating_shielded_supply() const;
    Amount get_fee_pool_reserve() const;
    Amount get_accumulated_fees() const { return get_fee_pool_reserve(); }

    // Verificación matemática de solvencia estricta (1:1 backing)
    // Collateral == Circulating Supply + Pool Reserve
    bool audit_solvency() const;

    uint32_t get_deposit_fee_bps() const { return deposit_fee_bps_; }
    uint32_t get_withdraw_fee_bps() const { return withdraw_fee_bps_; }

    void set_fee_bps(uint32_t deposit_bps, uint32_t withdraw_bps);
    void restore_state(Amount collateral, Amount circulating, Amount fee_pool);

private:
    mutable std::mutex mutex_;
    Amount total_collateral_{0};         // USDT totales bloqueados en la bóveda
    Amount circulating_supply_{0};       // Tokens privados en circulación
    Amount fee_pool_reserve_{0};         // Reserva de comisiones acumuladas en el pool

    uint32_t deposit_fee_bps_{50};       // 50 bps = 0.50%
    uint32_t withdraw_fee_bps_{50};      // 50 bps = 0.50%
};

} // namespace crypto
