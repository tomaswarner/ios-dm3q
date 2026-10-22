/* TURNIP-IOS compat shim for <linux/types.h> */
#ifndef _TURNIP_IOS_LINUX_TYPES_H
#define _TURNIP_IOS_LINUX_TYPES_H
#include <stdint.h>
#include <sys/types.h>
typedef uint8_t __u8;  typedef int8_t __s8;
typedef uint16_t __u16; typedef int16_t __s16;
typedef uint32_t __u32; typedef int32_t __s32;
typedef uint64_t __u64; typedef int64_t __s64;
typedef unsigned long __kernel_size_t;   /* LP64 Linux arm64 == Darwin size_t (8 bytes) */
typedef long __kernel_ssize_t;
#endif
