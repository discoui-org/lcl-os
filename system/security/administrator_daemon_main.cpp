#include "system/security/administrator_daemon.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<bool> running{true};
void stop(int) { running.store(false); }
}

int main() {
    if (geteuid() != 0) {
        std::cerr << "[LCL Admin ERROR] lcl-admind must run as root\n";
        return 1;
    }
    lcl::security::AdministratorDaemon daemon;
    std::string error;
    if (!daemon.initialize(error)) {
        std::cerr << "[LCL Admin ERROR] " << error << '\n';
        return 1;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::signal(SIGPIPE, SIG_IGN);
    std::cout << "[LCL Admin] Ready\n";
    while (running.load()) {
        daemon.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    daemon.shutdown();
    return 0;
}
