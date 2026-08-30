#include <iostream>
#include <string>
#include <filesystem>
#include "system/javascript/js_runtime.hpp"

int main(int argc, char* argv[]) {
    std::string scriptPath = "main.js";

    if (argc >= 2) {
        scriptPath = argv[1];
    } else {
        if (std::filesystem::exists("bin/main.js")) {
            scriptPath = "bin/main.js";
        } else if (std::filesystem::exists("main.js")) {
            scriptPath = "main.js";
        }
    }

    std::cout << "====================================================\n"
              << "  LCL OS JavaScript Runtime (lcl-js v0.1.0)        \n"
              << "  Engine: QuickJS (ES2023 JS Engine + lcl-ui)      \n"
              << "====================================================\n"
              << "[lcl-js] Executing script: " << scriptPath << "\n";

    lcl::binding::JsRuntime js;
    if (!js.initialize()) {
        std::cerr << "[lcl-js ERROR] Failed to initialize QuickJS runtime.\n";
        return 1;
    }

    if (!js.evalFile(scriptPath)) {
        std::cerr << "[lcl-js ERROR] Script execution failed: " << scriptPath << "\n";
        return 1;
    }

    return 0;
}
