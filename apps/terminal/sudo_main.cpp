#include "system/security/administrator_client.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: sudo <command> [arguments...]\n"
                  << "interactive root shells are not supported\n";
        return 2;
    }
    std::array<char, 4096> workingDirectory{};
    if (!getcwd(workingDirectory.data(), workingDirectory.size())) {
        std::cerr << "sudo: " << std::strerror(errno) << '\n';
        return 125;
    }
    lcl::security::AdministratorExecuteRequest request;
    request.workingDirectory = workingDirectory.data();
    for (int index = 1; index < argc; ++index) request.arguments.emplace_back(argv[index]);

    lcl::security::AdministratorCommandClient client;
    std::string error;
    if (!client.connect(lcl::security::kAdministratorSocket, error)) {
        std::cerr << "sudo: " << error << '\n';
        return 125;
    }
    const int exitCode = client.execute(request, error);
    if (!error.empty()) std::cerr << "sudo: " << error << '\n';
    return exitCode;
}
