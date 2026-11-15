/* glslang < 12 (Ubuntu 22.04: 11.8) installs no Public/ResourceLimits.h;
 * its libglslang-default-resource-limits.a exports the same defaults. */
#pragma once
#include <glslang/Include/ResourceLimits.h>
namespace glslang { extern const TBuiltInResource DefaultTBuiltInResource; }
inline const TBuiltInResource* GetDefaultResources() { return &glslang::DefaultTBuiltInResource; }
