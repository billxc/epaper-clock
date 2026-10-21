#pragma once

/* Forced into FatFs and all its consumers by the root CMakeLists.txt.
 * Do not predefine FFCONF_DEF: it is a revision, not the SDK include guard.
 * The SDK's _FFCONF_DEFINED prevents ff.h from undoing this override later. */
#ifndef EPAPER_SDK_FFCONF_PATH
#error "Root CMake must provide the absolute SDK ffconf.h path"
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include EPAPER_SDK_FFCONF_PATH
#ifdef __cplusplus
}
#endif

#if !defined(_FFCONF_DEFINED) || FFCONF_DEF != 80286
#error "This exFAT integration requires ESP-IDF FatFs R0.15 ffconf.h"
#endif

#undef FF_FS_EXFAT
#define FF_FS_EXFAT 1

/* This exFAT branch uses FF_USE_LABEL in a C expression. Disabled Kconfig
 * booleans are absent, so supply the actual zero instead of an undefined name. */
#ifndef CONFIG_FATFS_USE_LABEL
#undef FF_USE_LABEL
#define FF_USE_LABEL 0
#endif

#if FF_USE_LFN != 3 || FF_MAX_LFN != 255 || FF_LFN_UNICODE != 2
#error "FatFs requires heap LFN buffers, MAX_LFN=255 and UTF-8 API encoding"
#endif
/* Keep the SDK's 32-bit LBA diskio ABI; 64-bit file sizes are independent. */
#if FF_LBA64 != 0
#error "SDK SDMMC diskio requires 32-bit LBAs"
#endif
