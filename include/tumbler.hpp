#pragma once

#include "types.hpp"
#include <vector>
#include <string>
#include <chrono>

namespace crypto {

struct MicroHop {
    std::string from_address;
    std::string to_address;
    Amount amount;
    uint32_t delay_ms;          // Retardo temporal asignado (Poisson jitter)
    std::string hop_tx_hash;
};

struct TumblerRoute {
    size_t fragment_index;
    Amount fragment_amount;
    std::vector<MicroHop> hops;
};

struct TumblingPlan {
    std::string order_id;
    Amount total_gross_tokens;
    Amount fee_deducted_to_pool;
    Amount total_net_usdt;
    std::string destination_address;
    size_t total_fragments;
    std::vector<TumblerRoute> routes;
    uint64_t total_estimated_duration_ms;
};

class TumblerEngine {
public:
    TumblerEngine(
        size_t min_fragments = 5,
        size_t max_fragments = 12,
        size_t min_hops = 2,
        size_t max_hops = 4,
        uint32_t mean_delay_ms = 250
    );

    // Planificar la fragmentación y enrutamiento en microtransacciones
    TumblingPlan plan_withdrawal_tumbling(
        const std::string& order_id,
        Amount gross_tokens,
        Amount fee_pool,
        Amount net_usdt,
        const std::string& destination_address
    );

    // Ejecutar el plan de mezclado con retardos asíncronos y saltos efímeros
    bool execute_tumbling_plan(
        const TumblingPlan& plan,
        bool simulate_delays = true
    );

private:
    size_t min_fragments_;
    size_t max_fragments_;
    size_t min_hops_;
    size_t max_hops_;
    uint32_t mean_delay_ms_;

    // Algoritmo de particionado exacto sin pérdida de micro-centavos
    std::vector<Amount> partition_amount(Amount total_amount, size_t count);
};

} // namespace crypto
