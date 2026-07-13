// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <span>

#include "common/common_types.h"

namespace Vulkan {

enum class ScreenshotPixelFormat {
    Rgba8,
    Bgra8,
};

void ConvertScreenshotToQImageFormat(std::span<u8> pixels, ScreenshotPixelFormat source_format);

} // namespace Vulkan
