#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#include <asm/types.h>

#define EDU_IOCTL_BASE 'e'

#define EDU_IOCTL_IDENT           _IOR(EDU_IOCTL_BASE, 1, __u32)
#define EDU_IOCTL_LIVENESS        _IOWR(EDU_IOCTL_BASE, 2, __u32)
#define EDU_IOCTL_FACTORIAL       _IOWR(EDU_IOCTL_BASE, 3, __u32)
#define EDU_IOCTL_WAIT_IRQ        _IOR(EDU_IOCTL_BASE, 4, __u32)
#define EDU_IOCTL_RAISE_IRQ       _IOC(_IOC_WRITE, EDU_IOCTL_BASE, 5, 0)
#define EDU_IOCTL_DMA_TO_DEVICE   _IOC(_IOC_WRITE, EDU_IOCTL_BASE, 6, 0)
#define EDU_IOCTL_DMA_FROM_DEVICE _IOC(_IOC_WRITE, EDU_IOCTL_BASE, 7, 0)

#define EDU_DMA_BUF_SIZE 4096
