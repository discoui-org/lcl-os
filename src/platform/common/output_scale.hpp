#pragma once

namespace lcl::platform {

/** Clamp and snap a configured output scale without retaining global state. */
float sanitizeOutputScale(float scale) noexcept;

} // namespace lcl::platform
