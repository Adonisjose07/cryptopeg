#include "tumbler.hpp"
#include <random>
#include <thread>
#include <iostream>
#include <numeric>

namespace crypto {

TumblerEngine::TumblerEngine(
    size_t min_fragments,
    size_t max_fragments,
    size_t min_hops,
    size_t max_hops,
    uint32_t mean_delay_ms
) : min_fragments_(min_fragments),
    max_fragments_(max_fragments),
    min_hops_(min_hops),
    max_hops_(max_hops),
    mean_delay_ms_(mean_delay_ms) {}

std::vector<Amount> TumblerEngine::partition_amount(Amount total_amount, size_t count) {
    if (count <= 1 || total_amount < count) {
        return {total_amount};
    }

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_real_distribution<double> dist(1.0, 10.0);

    std::vector<double> weights(count);
    double sum_weights = 0.0;
    for (size_t i = 0; i < count; ++i) {
        weights[i] = dist(gen);
        sum_weights += weights[i];
    }

    std::vector<Amount> fragments(count);
    Amount running_sum = 0;

    for (size_t i = 0; i < count - 1; ++i) {
        double ratio = weights[i] / sum_weights;
        Amount part = static_cast<Amount>(total_amount * ratio);
        if (part == 0) part = 1;
        fragments[i] = part;
        running_sum += part;
    }

    // El último fragmento absorbe la diferencia exacta para garantizar 0% de redondeo
    if (running_sum < total_amount) {
        fragments[count - 1] = total_amount - running_sum;
    } else {
        // En caso excepcional de sobregiro por redondeo hacia arriba
        fragments[count - 1] = 1;
        fragments[0] = fragments[0] - (running_sum + 1 - total_amount);
    }

    return fragments;
}

static std::string generate_ephemeral_address() {
    uint8_t rand_bytes[20];
    randombytes_buf(rand_bytes, 20);
    return "0x" + to_hex(rand_bytes, 20);
}

static std::string generate_tx_hash() {
    uint8_t rand_bytes[32];
    randombytes_buf(rand_bytes, 32);
    return "0x" + to_hex(rand_bytes, 32);
}

TumblingPlan TumblerEngine::plan_withdrawal_tumbling(
    const std::string& order_id,
    Amount gross_tokens,
    Amount fee_pool,
    Amount net_usdt,
    const std::string& destination_address
) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> frag_dist(min_fragments_, max_fragments_);
    std::uniform_int_distribution<size_t> hop_dist(min_hops_, max_hops_);
    std::exponential_distribution<double> delay_dist(1.0 / static_cast<double>(mean_delay_ms_));

    size_t k = frag_dist(gen);
    std::vector<Amount> parts = partition_amount(net_usdt, k);

    TumblingPlan plan;
    plan.order_id = order_id;
    plan.total_gross_tokens = gross_tokens;
    plan.fee_deducted_to_pool = fee_pool;
    plan.total_net_usdt = net_usdt;
    plan.destination_address = destination_address;
    plan.total_fragments = parts.size();
    plan.total_estimated_duration_ms = 0;

    std::string pool_vault_origin = "0xVaultCollateralReserveUSDT";

    for (size_t i = 0; i < parts.size(); ++i) {
        TumblerRoute route;
        route.fragment_index = i + 1;
        route.fragment_amount = parts[i];

        size_t hops_count = hop_dist(gen);
        std::string current_source = pool_vault_origin;

        uint64_t route_duration = 0;
        for (size_t h = 0; h < hops_count; ++h) {
            MicroHop hop;
            hop.from_address = current_source;
            if (h == hops_count - 1) {
                // Último salto llega al destino final
                hop.to_address = destination_address;
            } else {
                hop.to_address = generate_ephemeral_address();
            }

            hop.amount = parts[i];
            hop.delay_ms = std::max(50u, static_cast<uint32_t>(delay_dist(gen)));
            hop.hop_tx_hash = generate_tx_hash();

            current_source = hop.to_address;
            route_duration += hop.delay_ms;
            route.hops.push_back(hop);
        }

        plan.total_estimated_duration_ms = std::max(plan.total_estimated_duration_ms, route_duration);
        plan.routes.push_back(route);
    }

    return plan;
}

bool TumblerEngine::execute_tumbling_plan(
    const TumblingPlan& plan,
    bool simulate_delays
) {
    std::cout << "\n======================================================\n";
    std::cout << "[TUMBLER] Ejecutando orden de retiro anonimizada: " << plan.order_id << "\n";
    std::cout << "[TUMBLER] Monto Neto a dispersar: " << format_usdt(plan.total_net_usdt) << "\n";
    std::cout << "[TUMBLER] Microtransacciones generadas: " << plan.total_fragments << " lotes\n";
    std::cout << "[TUMBLER] Destino final: " << plan.destination_address << "\n";
    std::cout << "======================================================\n";

    Amount total_disbursed = 0;

    for (const auto& route : plan.routes) {
        std::cout << " -> [Fragmento #" << route.fragment_index << "] "
                  << format_usdt(route.fragment_amount)
                  << " (" << route.hops.size() << " saltos efimeros programados)\n";

        for (size_t h = 0; h < route.hops.size(); ++h) {
            const auto& hop = route.hops[h];
            if (simulate_delays) {
                // Pequeño retardo representativo para la simulación
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min(hop.delay_ms / 10, 50u)));
            }

            std::cout << "    Salto " << (h + 1) << ": "
                      << hop.from_address.substr(0, 10) << "... -> "
                      << hop.to_address.substr(0, 10) << "... | "
                      << "Tx: " << hop.hop_tx_hash.substr(0, 14) << "... | "
                      << "Delay: +" << hop.delay_ms << "ms\n";
        }
        total_disbursed += route.fragment_amount;
    }

    std::cout << "[TUMBLER] Total final reensamblado y entregado: " << format_usdt(total_disbursed) << "\n";
    return (total_disbursed == plan.total_net_usdt);
}

} // namespace crypto
