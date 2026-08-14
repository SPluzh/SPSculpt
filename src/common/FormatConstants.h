#pragma once

namespace Format {
    // Current native project format
    constexpr const char* PROJECT_EXT       = "spsculpt";
    constexpr const char* PROJECT_DOT_EXT   = ".spsculpt";
    constexpr const char* PROJECT_FILTER    = "*.spsculpt";
    constexpr const char* PROJECT_NAME      = "SPSculpt Project";
    constexpr int         CURRENT_VERSION   = 14;

    // Legacy extensions for backward-compatible import
    constexpr const char* LEGACY_EXT        = "sgl";
    constexpr const char* LEGACY_DOT_EXT    = ".sgl";
}
