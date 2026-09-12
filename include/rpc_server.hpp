#pragma once

#include "node.hpp"
#include <string>
#include <memory>
#include <thread>
#include <atomic>

namespace httplib {
    class Server;
}

namespace crypto {

class P2PManager;

class RpcServer {
public:
    RpcServer(Node& node, const std::string& host = "0.0.0.0", int port = 8080);
    ~RpcServer();

    // Iniciar servidor de manera bloqueante (para el proceso principal daemon)
    void listen();

    // Iniciar servidor en un hilo secundario (para pruebas o tareas en background)
    void start_async();

    // Detener servidor
    void stop();

    void set_p2p_manager(P2PManager* p2p) { p2p_manager_ = p2p; }
    P2PManager* get_p2p_manager() const { return p2p_manager_; }

    bool is_running() const { return is_running_; }
    int get_port() const { return port_; }
    const std::string& get_host() const { return host_; }

private:
    void setup_routes();

    Node& node_;
    std::string host_;
    int port_;
    P2PManager* p2p_manager_{nullptr};
    std::unique_ptr<httplib::Server> server_;
    std::thread worker_thread_;
    std::atomic<bool> is_running_{false};
};

} // namespace crypto
