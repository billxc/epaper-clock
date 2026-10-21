/* Host-only FatFs R0.15 (revision 80286) configuration.
 * Force-include in EVERY translation unit, including the unmodified SDK ff.c
 * and ffunicode.c. _FFCONF_DEFINED skips ESP-IDF's sdkconfig/FreeRTOS ffconf.h.
 * ff.h checks FFCONF_DEF: do not silently use this with another revision.
 * This defines a separate host ABI, NOT the target SDK configuration or ABI.
 * Single process/thread, one regular image, 512-byte sectors, no formatting.
 * SDK sources are linked from $IDF_PATH, not copied into this repository;
 * their ChaN copyright/license notices remain in those original source files.
 */
#ifndef EPAPER_FATFS_HOST_CONFIG_H
#define EPAPER_FATFS_HOST_CONFIG_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#ifdef _FFCONF_DEFINED
#error Force-include fatfs_host_config.h before any FatFs headers
#endif
#define _FFCONF_DEFINED
#define FFCONF_DEF 80286

#define FF_FS_READONLY 0
#define FF_FS_MINIMIZE 0
#define FF_USE_FIND 0
#define FF_USE_MKFS 0
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND 1
#define FF_USE_CHMOD 0
#define FF_USE_LABEL 0
#define FF_USE_FORWARD 0
#define FF_USE_STRFUNC 0
#define FF_PRINT_LLI 0
#define FF_PRINT_FLOAT 0
#define FF_STRF_ENCODE 3

#define FF_CODE_PAGE 437
#define FF_USE_LFN 3
#define FF_MAX_LFN 255
#define FF_LFN_UNICODE 2
#define FF_LFN_BUF 255
#define FF_SFN_BUF 12
#define FF_FS_RPATH 0

#define FF_VOLUMES 1
#define FF_STR_VOLUME_ID 0
#define FF_VOLUME_STRS "HOST"
/* 0: physical drive 0, auto-discover either superfloppy or MBR partition. */
#define FF_MULTI_PARTITION 0
#define FF_MIN_SS 512
#define FF_MAX_SS 512
#define FF_LBA64 0
#define FF_MIN_GPT 0x10000000
#define FF_USE_TRIM 0

#define FF_FS_TINY 0
#define FF_FS_EXFAT 1
#define FF_FS_NORTC 0
#define FF_NORTC_MON 1
#define FF_NORTC_MDAY 1
#define FF_NORTC_YEAR 2024
#define FF_FS_NOFSINFO 0
#define FF_FS_LOCK 0
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT 1000
/* ESP-IDF extension, deliberately static sector buffers for this host ABI. */
#define FF_USE_DYN_BUFFER 0

#endif
