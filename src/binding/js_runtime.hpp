#pragma once

#include <string>
#include <memory>
#include "quickjs.h"

namespace lcl::binding {

class JsRuntime {
public:
    JsRuntime();
    ~JsRuntime();

    // Non-copyable
    JsRuntime(const JsRuntime&) = delete;
    JsRuntime& operator=(const JsRuntime&) = delete;

    /**
     * @brief Initialize QuickJS engine, register LCL UI bindings, and console.log
     */
    bool initialize();

    /**
     * @brief Evaluate a JavaScript source file.
     */
    bool evalFile(const std::string& filePath);

    /**
     * @brief Evaluate JavaScript code string.
     */
    bool evalCode(const std::string& code, const std::string& filename = "<input>");

    /**
     * @brief Clean up QuickJS context and runtime.
     */
    void shutdown();

    JSContext* getContext() const { return m_ctx; }
    JSRuntime* getRuntime() const { return m_rt; }

    static void printException(JSContext* ctx);

private:
    void registerLclBindings();

    JSRuntime* m_rt{nullptr};
    JSContext* m_ctx{nullptr};
    bool m_initialized{false};
};

} // namespace lcl::binding
