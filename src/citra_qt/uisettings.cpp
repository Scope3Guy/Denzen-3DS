// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "uisettings.h"

#include "common/file_util.h"

namespace UISettings {

const Themes themes{{
    {"System", "default"},
    {"System With Colorful Icons", "colorful"},
    {"Dark", "qdarkstyle"},
    {"Dark Colorful", "colorful_dark"},
    {"Midnight Blue", "qdarkstyle_midnight_blue"},
    {"Midnight Blue Colorful", "colorful_midnight_blue"},
}};

std::string NormalizeScreenshotPath(std::string_view path) {
    if (path.empty()) {
        return FileUtil::SanitizePath(
            FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "screenshots");
    }

    return FileUtil::SanitizePath(path);
}

Values values = {};
} // namespace UISettings
