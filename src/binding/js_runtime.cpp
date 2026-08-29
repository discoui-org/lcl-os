#include "binding/js_runtime.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include "binding/js_binding_registry.hpp"

namespace lcl::binding {

JsRuntime::JsRuntime() = default;

JsRuntime::~JsRuntime() {
    shutdown();
}

bool JsRuntime::initialize() {
    if (m_initialized) return true;

    m_rt = JS_NewRuntime();
    if (!m_rt) {
        std::cerr << "[LCL JS ERROR] Failed to create QuickJS runtime.\n";
        return false;
    }

    m_ctx = JS_NewContext(m_rt);
    if (!m_ctx) {
        std::cerr << "[LCL JS ERROR] Failed to create QuickJS context.\n";
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
        return false;
    }

    registerLclBindings(m_ctx, m_rt);

    m_initialized = true;
    std::cout << "[LCL JS] QuickJS runtime initialized successfully.\n";
    return true;
}

bool JsRuntime::evalFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "[LCL JS ERROR] Cannot open JS file: " << filePath << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return evalCode(buffer.str(), filePath);
}

bool JsRuntime::evalCode(const std::string& code, const std::string& filename) {
    if (!m_initialized && !initialize()) return false;

    JSValue value = JS_Eval(m_ctx, code.c_str(), code.size(), filename.c_str(),
                            JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        printException(m_ctx);
        JS_FreeValue(m_ctx, value);
        return false;
    }

    JS_FreeValue(m_ctx, value);
    return true;
}

void JsRuntime::printException(JSContext* ctx) {
    JSValue exception = JS_GetException(ctx);
    const char* message = JS_ToCString(ctx, exception);
    if (message) {
        std::cerr << "[LCL JS Exception] " << message << std::endl;
        JS_FreeCString(ctx, message);
    }
    JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
    if (!JS_IsUndefined(stack)) {
        const char* stackText = JS_ToCString(ctx, stack);
        if (stackText) {
            std::cerr << "[LCL JS Stack] " << stackText << std::endl;
            JS_FreeCString(ctx, stackText);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exception);
}

void JsRuntime::shutdown() {
    if (!m_initialized) return;

    if (m_ctx) {
        JS_RunGC(m_rt);
        JS_FreeContext(m_ctx);
        m_ctx = nullptr;
    }
    if (m_rt) {
        JS_RunGC(m_rt);
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
    }
    m_initialized = false;
    std::cout << "[LCL JS] QuickJS runtime shutdown complete.\n";
}

} // namespace lcl::binding
