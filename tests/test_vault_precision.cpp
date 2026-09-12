#include "vault.hpp"
#include <cassert>
#include <iostream>
#include <random>

int main() {
    std::cout << "[TEST] Iniciando prueba de precision matematica y solvencia 1:1 de la Boveda...\n";

    crypto::Vault vault(50, 50); // 0.50% comisiones
    assert(vault.audit_solvency());

    // 1. Depósito simple
    crypto::Amount deposit_100 = 100 * crypto::USDT_UNIT;
    auto dep_receipt = vault.deposit(deposit_100);

    // 0.5% de 100 USDT = 0.50 USDT (500,000 micro-USDT)
    assert(dep_receipt.fee_to_pool == 500'000ULL);
    assert(dep_receipt.net_shielded_tokens_minted == 99'500'000ULL);
    assert(vault.get_total_collateral() == 100'000'000ULL);
    assert(vault.get_fee_pool_reserve() == 500'000ULL);
    assert(vault.get_circulating_shielded_supply() == 99'500'000ULL);
    assert(vault.audit_solvency());
    std::cout << "  -> Deposito unitario verificado con exito.\n";

    // 2. Retiro simple
    crypto::Amount withdraw_50 = 50 * crypto::USDT_UNIT;
    auto w_receipt = vault.request_withdrawal(withdraw_50);

    // 0.5% de 50 USDT = 0.25 USDT (250,000 micro-USDT)
    assert(w_receipt.fee_to_pool == 250'000ULL);
    assert(w_receipt.net_usdt_to_tumble == 49'750'000ULL);
    assert(vault.audit_solvency());
    std::cout << "  -> Retiro unitario verificado con exito.\n";

    // 3. Test de Estrés: 10,000 transacciones aleatorias con montos fraccionarios
    std::cout << "  -> Ejecutando test de estres con 10,000 transacciones aleatorias...\n";
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<crypto::Amount> dist(100, 100'000'000ULL); // desde 0.000100 a 100 USDT

    for (int i = 0; i < 5000; ++i) {
        crypto::Amount dep = dist(rng);
        vault.deposit(dep);
        assert(vault.audit_solvency());

        crypto::Amount current_supply = vault.get_circulating_shielded_supply();
        if (current_supply > 1000) {
            std::uniform_int_distribution<crypto::Amount> w_dist(1, current_supply / 2);
            crypto::Amount w_amt = w_dist(rng);
            vault.request_withdrawal(w_amt);
            assert(vault.audit_solvency());
        }
    }

    assert(vault.audit_solvency());
    std::cout << "  -> Colateral final: " << crypto::format_usdt(vault.get_total_collateral()) << "\n";
    std::cout << "  -> Suministro circulante: " << crypto::format_usdt(vault.get_circulating_shielded_supply()) << "\n";
    std::cout << "  -> Comisiones en pool: " << crypto::format_usdt(vault.get_fee_pool_reserve()) << "\n";
    std::cout << "[TEST PASSED] Precision decimal absoluta y balance 1:1 verificado sin margen de error.\n";

    return 0;
}
