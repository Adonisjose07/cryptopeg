#include "vault.hpp"
#include <stdexcept>
#include <random>

namespace crypto {

Vault::Vault(uint32_t deposit_fee_bps, uint32_t withdraw_fee_bps)
    : deposit_fee_bps_(deposit_fee_bps), withdraw_fee_bps_(withdraw_fee_bps) {
    if (sodium_init() < 0) {
        throw std::runtime_error("Fallo al inicializar libsodium.");
    }
}

DepositReceipt Vault::deposit(Amount usdt_gross) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (usdt_gross == 0) {
        throw std::invalid_argument("El depósito debe ser mayor a 0.");
    }

    // Cálculo exacto en punto fijo de la comisión: (Monto * bps) / 10000
    Amount fee = (usdt_gross * deposit_fee_bps_) / 10000ULL;
    Amount net_minted = usdt_gross - fee;

    // Actualización de balances
    total_collateral_ += usdt_gross;
    circulating_supply_ += net_minted;
    fee_pool_reserve_ += fee;

    // Invariante de seguridad
    if (total_collateral_ != (circulating_supply_ + fee_pool_reserve_)) {
        throw std::runtime_error("Violación crítica de invariante en depósito de bóveda.");
    }

    // Generar hash de transacción simulado
    uint8_t random_hash[32];
    randombytes_buf(random_hash, 32);

    DepositReceipt receipt;
    receipt.gross_usdt_deposited = usdt_gross;
    receipt.fee_to_pool = fee;
    receipt.net_shielded_tokens_minted = net_minted;
    receipt.tx_hash = "0x" + to_hex(random_hash, 32);

    return receipt;
}

WithdrawalReceipt Vault::request_withdrawal(Amount tokens_gross) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (tokens_gross == 0) {
        throw std::invalid_argument("La cantidad de tokens a retirar debe ser mayor a 0.");
    }

    if (tokens_gross > circulating_supply_) {
        throw std::runtime_error("Fondos insuficientes en suministro circulante para quemar.");
    }

    // Comisión de salida al pool
    Amount fee = (tokens_gross * withdraw_fee_bps_) / 10000ULL;
    Amount net_usdt = tokens_gross - fee;

    if (net_usdt > total_collateral_) {
        throw std::runtime_error("Bóveda insolvente: colateral insuficiente.");
    }

    // Quema de tokens y liberación de colateral
    circulating_supply_ -= tokens_gross;
    total_collateral_ -= net_usdt;
    fee_pool_reserve_ += fee; // La comisión queda en el pool dentro de la bóveda

    // Invariante de solvencia
    if (total_collateral_ != (circulating_supply_ + fee_pool_reserve_)) {
        throw std::runtime_error("Violación crítica de invariante en retiro de bóveda.");
    }

    uint8_t order_bytes[16];
    randombytes_buf(order_bytes, 16);

    WithdrawalReceipt receipt;
    receipt.gross_tokens_burned = tokens_gross;
    receipt.fee_to_pool = fee;
    receipt.net_usdt_to_tumble = net_usdt;
    receipt.order_id = "ORD-" + to_hex(order_bytes, 16);

    return receipt;
}

Amount Vault::get_total_collateral() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_collateral_;
}

Amount Vault::get_circulating_shielded_supply() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return circulating_supply_;
}

Amount Vault::get_fee_pool_reserve() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fee_pool_reserve_;
}

bool Vault::audit_solvency() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_collateral_ == (circulating_supply_ + fee_pool_reserve_);
}

void Vault::set_fee_bps(uint32_t deposit_bps, uint32_t withdraw_bps) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deposit_bps > 1000 || withdraw_bps > 1000) { // máx 10%
        throw std::invalid_argument("La comisión no puede exceder el 10% (1000 bps).");
    }
    deposit_fee_bps_ = deposit_bps;
    withdraw_fee_bps_ = withdraw_bps;
}

void Vault::restore_state(Amount collateral, Amount circulating, Amount fee_pool) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_collateral_ = collateral;
    circulating_supply_ = circulating;
    fee_pool_reserve_ = fee_pool;
    if (total_collateral_ != (circulating_supply_ + fee_pool_reserve_)) {
        throw std::runtime_error("Fallo al restaurar estado de bóveda: inconsistencia en colateral 1:1.");
    }
}

} // namespace crypto
