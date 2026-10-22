/* TURNIP-IOS compat shim: Linux UAPI <linux/dma-heap.h> (the struct/ioctl layout is Linux's ABI).
 * Only compiled in so tu_knl_kgsl.cc builds; no dma-heap exists on iOS, the device open of
 * /dev/dma_heap/system fails at runtime (see TURNIP-IOS.md for the Mesa patch that routes around it). */
#ifndef _TURNIP_IOS_LINUX_DMA_HEAP_H
#define _TURNIP_IOS_LINUX_DMA_HEAP_H
#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/types.h>



struct dma_heap_allocation_data {
   __u64 len;
   __u32 fd;
   __u32 fd_flags;
   __u64 heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif
