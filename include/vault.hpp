#pragma once

#include "types.hpp"
#include <mutex>
#include <string>

namespace crypto {

struct DepositReceipt {
    Amount gross_usdt_deposited;
    Amount fee_to_pool;
    Amount net_shielded_tokens_minted;
    std::string tx_hash;
};

struct WithdrawalReceipt {
    Amount gross_tokens_burned;
    Amount fee_to_pool;
    Amount net_usdt_to_tumble;
    std::string order_id;
    std::string destination_address;
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
