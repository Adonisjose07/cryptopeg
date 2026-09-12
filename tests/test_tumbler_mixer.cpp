#include "tumbler.hpp"
#include <cassert>
#include <iostream>

int main() {
    std::cout << "[TEST] Iniciando prueba del motor mezclador de microtransacciones (Tumbler)...\n";

    crypto::TumblerEngine tumbler(5, 12, 2, 4, 150);

    // Retiro de 345.678901 USDT
    crypto::Amount gross = 345'678'901ULL;
    crypto::Amount fee = (gross * 50ULL) / 10000ULL; // 0.5%
    crypto::Amount net = gross - fee;
    std::string destination = "0xUserDestinationColdWalletAddress987654";

    std::cout << "  -> Planificando retiro anonimizado para " << crypto::format_usdt(net) << "...\n";
    auto plan = tumbler.plan_withdrawal_tumbling(
        "ORD-TEST-001",
        gross,
        fee,
        net,
        destination
    );

    assert(plan.total_net_usdt == net);
    assert(plan.routes.size() >= 5 && plan.routes.size() <= 12);
    std::cout << "  -> Plan generado con " << plan.routes.size() << " microtransacciones fraccionadas.\n";

    // Verificar que la suma de todos los fragmentos sea EXACTA al micro-centavo
    crypto::Amount sum_fragments = 0;
    for (const auto& route : plan.routes) {
        sum_fragments += route.fragment_amount;
        assert(!route.hops.empty());
        // El último salto debe apuntar exactamente a la billetera destino
        assert(route.hops.back().to_address == destination);
        // Cada salto debe tener un delay programado
        for (const auto& hop : route.hops) {
            assert(hop.delay_ms >= 50);
            assert(!hop.hop_tx_hash.empty());
        }
    }

    assert(sum_fragments == net);
    std::cout << "  -> Suma de fragmentos (" << crypto::format_usdt(sum_fragments)
              << ") coincide EXACTAMENTE con el monto neto ("
              << crypto::format_usdt(net) << "). Cero perdida de redondeo.\n";

    // Ejecutar la simulación del mezclador
    bool executed = tumbler.execute_tumbling_plan(plan, false);
    assert(executed);
    std::cout << "[TEST PASSED] Motor de mezclado y dispersion de microtransacciones verificado con exito.\n";

    return 0;
}
