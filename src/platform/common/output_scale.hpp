#pragma once

namespace lcl::platform {

/** Clamp and snap a configured output scale without retaining global state. */
float sanitizeOutputScale(float scale) noexcept;

/** Resolve the process output-scale default from LCL_SCALE or lcl.scale=. */
float resolveOutputScale();

} // namespace lcl::platform
