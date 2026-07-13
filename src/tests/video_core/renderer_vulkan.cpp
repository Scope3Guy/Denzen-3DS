// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef ENABLE_VULKAN

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/screenshot_conversion.h"

TEST_CASE("Vulkan screenshot channel conversion", "[video_core][vulkan]") {
    SECTION("RGBA source is converted to QImage byte order") {
        std::array<u8, 8> pixels{1, 2, 3, 4, 5, 6, 7, 8};
        Vulkan::ConvertScreenshotToQImageFormat(pixels, Vulkan::ScreenshotPixelFormat::Rgba8);
        const std::array<u8, 8> expected{3, 2, 1, 4, 7, 6, 5, 8};
        REQUIRE(pixels == expected);
    }

    SECTION("BGRA source is already in QImage byte order") {
        std::array<u8, 4> pixels{1, 2, 3, 4};
        Vulkan::ConvertScreenshotToQImageFormat(pixels, Vulkan::ScreenshotPixelFormat::Bgra8);
        const std::array<u8, 4> expected{1, 2, 3, 4};
        REQUIRE(pixels == expected);
    }

    SECTION("Incomplete trailing pixel data is not accessed") {
        std::array<u8, 3> pixels{1, 2, 3};
        Vulkan::ConvertScreenshotToQImageFormat(pixels, Vulkan::ScreenshotPixelFormat::Rgba8);
        const std::array<u8, 3> expected{1, 2, 3};
        REQUIRE(pixels == expected);
    }
}

#endif