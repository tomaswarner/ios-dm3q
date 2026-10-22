/* TURNIP-IOS compat shim for <linux/ioctl.h>: Darwin's <sys/ioctl.h> provides _IO/_IOR/_IOW/_IOWR
 * with the same size<<16 | type<<8 | nr layout as Linux; only the direction bits differ.
 * NOTE: Darwin _IOW sets IOC_IN=0x80000000 (Linux 0x40000000); see TURNIP-IOS.md section 4.1. */
#ifndef _TURNIP_IOS_LINUX_IOCTL_H
#define _TURNIP_IOS_LINUX_IOCTL_H
#include <sys/ioctl.h>
#endif
