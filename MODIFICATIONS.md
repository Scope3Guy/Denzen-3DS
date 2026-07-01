# Modifications From Upstream

Denzen is a modified fork of Azahar, which itself derives from Citra.

This file records tracked files that differ from the upstream Azahar source tree for the pre-public-history Denzen work. It is intended as a public change-notice companion for compliance with the GPL 2.0 license. Since initial work was experimental and local without a remote repo it was not tracked in git commit history. All future work will be tracked and available in Git commit history.

Comparison basis:

- Upstream: `azahar-emu/azahar` `master` at `d93adebb16a1137e3dc0a5afb627ef652ff6b569`, fetched on 2026-07-01.
- Denzen: public `master` source tree after the 2026-07-01 cleanup that removes internal local-only tooling from the public tree.
- Scope: tracked top-level source tree files only.
- Date precision: where private pre-remote work dates are not available, the date is recorded as `2026-06-30 or earlier`, matching the public baseline/import date and local development log evidence.

## Content or New-File Differences

| Status | File | Date changed | Notice |
|---|---|---:|---|
| M | `.ci/mxe.sh` | 2026-07-01 | Modified by Denzen project. |
| M | `.ci/windows.sh` | 2026-07-01 | Modified by Denzen project. |
| M | `.github/workflows/build.yml` | 2026-06-30 | Modified by Denzen project. |
| M | `.github/workflows/format.yml` | 2026-06-30 | Modified by Denzen project. |
| M | `.github/workflows/libretro.yml` | 2026-06-30 | Modified by Denzen project. |
| M | `.github/workflows/transifex.yml` | 2026-06-30 | Modified by Denzen project. |
| A | `.github/workflows/windows-test-build.yml` | 2026-06-30; 2026-07-01 | Added and modified by Denzen project. |
| M | `.gitignore` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `README.md` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `dist/languages/.tx/config` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/citra_qt/citra_qt.cpp` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/citra_qt/citra_qt.h` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/core/CMakeLists.txt` | 2026-06-29; 2026-06-30 | Modified by Denzen project. |
| M | `src/core/file_sys/title_metadata.cpp` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/core/file_sys/title_metadata.h` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/core/hle/service/am/am.cpp` | 2026-06-29; 2026-06-30 | Modified by Denzen project. |
| M | `src/core/hle/service/am/am.h` | 2026-06-30 | Modified by Denzen project. |
| M | `src/core/hle/service/am/am_net.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/am/am_sys.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/am/am_u.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/apt/apt.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/apt/apt.h` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/apt/ns_s.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/cfg/cfg_defaults.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/cfg/cfg_nor.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/cfg/cfg_nor.h` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/fs/fs_user.cpp` | 2026-06-30 | Modified by Denzen project. |
| A | `src/core/hle/service/hb/hb_ldr.cpp` | 2026-06-29 | Added by Denzen project. |
| A | `src/core/hle/service/hb/hb_ldr.h` | 2026-06-29 | Added by Denzen project. |
| M | `src/core/hle/service/http/http_c.cpp` | 2026-06-29; 2026-06-30 | Modified by Denzen project. |
| M | `src/core/hle/service/http/http_c.h` | 2026-06-29; 2026-06-30 | Modified by Denzen project. |
| M | `src/core/hle/service/mcu/mcu_hwc.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/mcu/mcu_hwc.h` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/nwm/nwm.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/nwm/nwm_ext.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/nwm/nwm_ext.h` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/ptm/ptm.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/ptm/ptm.h` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/ptm/ptm_sysm.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/hle/service/service.cpp` | 2026-06-29 | Modified by Denzen project. |
| M | `src/core/nus_download.cpp` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/video_core/rasterizer_cache/rasterizer_cache.h` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/video_core/rasterizer_cache/utils.h` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/video_core/renderer_opengl/gl_blit_helper.cpp` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/video_core/renderer_opengl/gl_rasterizer.cpp` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/video_core/renderer_opengl/gl_texture_runtime.cpp` | 2026-06-30 or earlier | Modified by Denzen project. |
| M | `src/video_core/renderer_vulkan/vk_texture_runtime.cpp` | 2026-06-30 or earlier | Modified by Denzen project. |

## Mode-Only Differences

These tracked files have executable-mode metadata differences from the upstream comparison tree, with no content-line difference in this comparison.

| Status | File | Date changed | Notice |
|---|---|---:|---|
| M | `.ci/android.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `.ci/clang-format.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `.ci/docker.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `.ci/ios.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `.ci/libretro-pack.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `.ci/license-header.rb` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `.ci/linux.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `.ci/macos-universal.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `.ci/macos.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `.ci/source.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `.ci/transifex.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `hooks/pre-commit` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `src/android/gradlew` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `src/input_common/analog_from_button.cpp` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `src/input_common/analog_from_button.h` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `tools/check-kotlin-formatting.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `tools/delete-ignored-files.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `tools/enter-docker-dev-container.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `tools/fix-kotlin-formatting.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `tools/purge-github-cache.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `tools/reset-submodules.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `tools/update-compatibility-list.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `tools/update-translations.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
| M | `tools/verify-release.sh` | 2026-06-30 or earlier | File mode differs in Denzen public baseline. |
