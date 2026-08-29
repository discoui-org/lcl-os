#pragma once

#include "quickjs.h"

namespace lcl::binding {

/** Register the native LCL namespace into one initialized QuickJS context. */
void registerLclBindings(JSContext* context, JSRuntime* runtime);

} // namespace lcl::binding
