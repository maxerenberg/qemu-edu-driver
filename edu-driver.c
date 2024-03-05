#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/cdev.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "edu.h"

// See https://github.com/qemu/qemu/blob/stable-7.2/docs/specs/edu.txt
#define PCI_VENDOR_ID_QEMU     0x1234
#define PCI_DEVICE_ID_QEMU_EDU 0x11e8

// The number of bits be changed in QEMU via '-device edu,dma_mask=<mask>'
#define EDU_DMA_BITS 28
#define EDU_DMA_BUF_DEVICE_OFFSET 0x40000
#define EDU_DMA_CMD_START_XFER    1
#define EDU_DMA_CMD_RAM_TO_DEVICE 0
#define EDU_DMA_CMD_DEVICE_TO_RAM 2
#define EDU_DMA_CMD_RAISE_IRQ     4
#define EDU_STATUS_COMPUTING 0x01
#define EDU_STATUS_RAISE_IRQ 0x80

#define EDU_ADDR_IDENT      0x0
#define EDU_ADDR_LIVENESS   0x04
#define EDU_ADDR_FACTORIAL  0x08
#define EDU_ADDR_STATUS     0x20
#define EDU_ADDR_IRQ_STATUS 0x24
#define EDU_ADDR_IRQ_RAISE  0x60
#define EDU_ADDR_IRQ_ACK    0x64
#define EDU_ADDR_DMA_SRC    0x80
#define EDU_ADDR_DMA_DST    0x88
#define EDU_ADDR_DMA_XFER   0x90
#define EDU_ADDR_DMA_CMD    0x98

static const struct pci_device_id edu_pci_tbl[] = {
    { PCI_DEVICE(PCI_VENDOR_ID_QEMU, PCI_DEVICE_ID_QEMU_EDU) },
    { }
};

// e.g. insmod edu.ko debug=1
// e.g. echo 1 > /sys/module/edu/parameters/debug
static bool param_debug;
module_param_named(debug, param_debug, bool, S_IRUGO | S_IWUSR);
#define edu_log(...) if (param_debug) pr_info(__VA_ARGS__)

// Load with msi=1 to use MSI instead of INTx
static bool param_msi;
module_param_named(msi, param_msi, bool, S_IRUGO);

struct edu_device {
    bool registered_irq_handler;
    bool added_cdev;
    struct cdev cdev;
    char *iomem;
    unsigned int irq;
    u32 irq_value;
    wait_queue_head_t irq_wait_queue;
    dma_addr_t dma_bus_addr;
    void *dma_virt_addr;
};

static dev_t devno;
static const int minor = 0;
static struct edu_device *edu_dev;

static int edu_open(struct inode *inode, struct file *filp) {
    struct edu_device *dev;

    nonseekable_open(inode, filp);
    dev = container_of(inode->i_cdev, struct edu_device, cdev);
    filp->private_data = dev;
    return 0;
}

static int edu_release(struct inode *inode, struct file *filp) {
    return 0;
}

static int edu_mmap(struct file *filp, struct vm_area_struct *vma) {
    struct edu_device *dev = filp->private_data;
    unsigned long len = vma->vm_end - vma->vm_start;

    if (len > EDU_DMA_BUF_SIZE) {
        return -EINVAL;
    }
    // Only allow offset=0
    if (vma->vm_pgoff) {
        return -EINVAL;
    }
    // VM_IO | VM_DONTEXPAND | VM_DONTDUMP are set by remap_pfn_range()
    return vm_iomap_memory(vma, __pa(dev->dma_virt_addr), len);
}

static int ioctl_ident(struct edu_device *dev, u32 __user *arg) {
    u32 val = ioread32(dev->iomem + EDU_ADDR_IDENT);
    return put_user(val, arg);
}

static int ioctl_liveness(struct edu_device *dev, u32 __user *arg) {
    u32 val;
    if (get_user(val, arg)) {
        return -EFAULT;
    }
    iowrite32(val, dev->iomem + EDU_ADDR_LIVENESS);
    val = ioread32(dev->iomem + EDU_ADDR_LIVENESS);
    return put_user(val, arg);
}

static bool is_computing_factorial(struct edu_device *dev) {
    return ioread32(dev->iomem + EDU_ADDR_STATUS) & EDU_STATUS_COMPUTING;
}

static int ioctl_factorial(struct edu_device *dev, u32 __user *arg) {
    u32 val;
    if (get_user(val, arg)) {
        return -EFAULT;
    }
    // raise interrupt after finishing factorial computation
    iowrite32(EDU_STATUS_RAISE_IRQ, dev->iomem + EDU_ADDR_STATUS);
    edu_log("Writing %u to register\n", val);
    iowrite32(val, dev->iomem + EDU_ADDR_FACTORIAL);
    if (wait_event_interruptible(dev->irq_wait_queue, !is_computing_factorial(dev))) {
        return -ERESTARTSYS;
    }
    // read result
    val = ioread32(dev->iomem + EDU_ADDR_FACTORIAL);
    edu_log("Got factorial result: %u\n", val);
    return put_user(val, arg);
}

static int ioctl_wait_irq(struct edu_device *dev, u32 __user *arg) {
    DEFINE_WAIT(wait);
    prepare_to_wait(&dev->irq_wait_queue, &wait, TASK_INTERRUPTIBLE);
    schedule();
    finish_wait(&dev->irq_wait_queue, &wait);
    if (signal_pending(current)) {
        return -ERESTARTSYS;
    }
    return put_user(READ_ONCE(dev->irq_value), arg);
}

static int ioctl_raise_irq(struct edu_device *dev, u32 arg) {
    iowrite32(arg, dev->iomem + EDU_ADDR_IRQ_RAISE);
    return 0;
}

static bool is_doing_dma(struct edu_device *dev) {
    return ioread32(dev->iomem + EDU_ADDR_DMA_CMD) & EDU_DMA_CMD_START_XFER;
}

static int do_dma(struct edu_device *dev, u32 len, bool to_device) {
    u32 src, dst, cmd;
    if (len == 0 || len > EDU_DMA_BUF_SIZE) {
        return -EINVAL;
    }
    if (dev->dma_bus_addr > ~(u32)0) {
        pr_warn("DMA bus addr is greater than 32 bits, cannot use iowrite32\n");
        return -EOPNOTSUPP;
    }
    if (to_device) {
        src = (u32)dev->dma_bus_addr;
        dst = EDU_DMA_BUF_DEVICE_OFFSET;
        cmd = EDU_DMA_CMD_START_XFER | EDU_DMA_CMD_RAM_TO_DEVICE | EDU_DMA_CMD_RAISE_IRQ;
    } else {
        src = EDU_DMA_BUF_DEVICE_OFFSET;
        dst = (u32)dev->dma_bus_addr;
        cmd = EDU_DMA_CMD_START_XFER | EDU_DMA_CMD_DEVICE_TO_RAM | EDU_DMA_CMD_RAISE_IRQ;
    }
    edu_log("src=0x%08x dst=0x%08x len=%u\n", src, dst, len);
    iowrite32(src, dev->iomem + EDU_ADDR_DMA_SRC);
    iowrite32(dst, dev->iomem + EDU_ADDR_DMA_DST);
    iowrite32(len, dev->iomem + EDU_ADDR_DMA_XFER);
    iowrite32(cmd, dev->iomem + EDU_ADDR_DMA_CMD);
    if (wait_event_interruptible(dev->irq_wait_queue, !is_doing_dma(dev))) {
        return -ERESTARTSYS;
    }
    return 0;
}

static int ioctl_dma_to_device(struct edu_device *dev, u32 arg) {
    return do_dma(dev, arg, true);
}

static int ioctl_dma_from_device(struct edu_device *dev, u32 arg) {
    return do_dma(dev, arg, false);
}

static long edu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct edu_device *dev = filp->private_data;
    switch (cmd) {
        case EDU_IOCTL_IDENT:
            return ioctl_ident(dev, (u32 __user*)arg);
        case EDU_IOCTL_LIVENESS:
            return ioctl_liveness(dev, (u32 __user*)arg);
        case EDU_IOCTL_FACTORIAL:
            return ioctl_factorial(dev, (u32 __user*)arg);
        case EDU_IOCTL_WAIT_IRQ:
            return ioctl_wait_irq(dev, (u32 __user*)arg);
        case EDU_IOCTL_RAISE_IRQ:
            return ioctl_raise_irq(dev, (u32)arg);
        case EDU_IOCTL_DMA_TO_DEVICE:
            return ioctl_dma_to_device(dev, (u32)arg);
        case EDU_IOCTL_DMA_FROM_DEVICE:
            return ioctl_dma_from_device(dev, (u32)arg);
        default:
            return -ENOTTY;
    }
}

struct file_operations edu_fops = {
    .owner = THIS_MODULE,
    .open = edu_open,
    .release = edu_release,
    .unlocked_ioctl = edu_ioctl,
    .mmap = edu_mmap,
};

static void edu_dev_init(struct edu_device *dev) {
    cdev_init(&dev->cdev, &edu_fops);
    dev->cdev.owner = THIS_MODULE;
    init_waitqueue_head(&dev->irq_wait_queue);
}

static irqreturn_t edu_irq_handler(int irq, void *dev_id) {
    struct edu_device *dev = dev_id;
    u32 irq_value;

    // Read the value which raised the interrupt
    irq_value = ioread32(dev->iomem + EDU_ADDR_IRQ_STATUS);
    edu_log("irq_value = %u\n", irq_value);
    // Clear the interrupt
    iowrite32(irq_value, dev->iomem + EDU_ADDR_IRQ_ACK);
    // Wake up any tasks waiting on the queue
    WRITE_ONCE(dev->irq_value, irq_value);
    wake_up_interruptible(&dev->irq_wait_queue);
    return IRQ_HANDLED;
}

static void edu_pci_cleanup(struct pci_dev *pdev) {
    if (!edu_dev) {
        return;
    }
    if (edu_dev->added_cdev) {
        cdev_del(&edu_dev->cdev);
        edu_dev->added_cdev = false;
    }
    if (edu_dev->registered_irq_handler) {
        free_irq(edu_dev->irq, edu_dev);
        edu_dev->registered_irq_handler = false;
    }
    if (pci_dev_msi_enabled(pdev)) {
        pci_free_irq_vectors(pdev);
    }
}

static int edu_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent) {
    int err;
    int nvec;

    // enable PCI device
    err = pcim_enable_device(pdev);
    if (err) {
        goto fail;
    }

    // enable DMA (note that this is necessary for MSI)
    pci_set_master(pdev);
    err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(EDU_DMA_BITS));
    if (err) {
        goto fail;
    }

    // set up DMA mapping
    edu_dev->dma_virt_addr = dmam_alloc_coherent(
        &pdev->dev, EDU_DMA_BUF_SIZE, &edu_dev->dma_bus_addr, GFP_KERNEL);
    if (!edu_dev->dma_virt_addr) {
        err = -ENOMEM;
        goto fail;
    }
    edu_log("DMA bus addr = 0x%08lx\n", (unsigned long)edu_dev->dma_bus_addr);
    edu_log("DMA virt addr = %p\n", edu_dev->dma_virt_addr);

    // request and iomap PCI BAR
    // There is one memory region, 1 MB in size
    edu_log("resource 0: start=0x%08llx end=0x%08llx\n", pci_resource_start(pdev, 0), pci_resource_end(pdev, 0));
    err = pcim_iomap_regions(pdev, BIT(0), KBUILD_MODNAME);
    if (err) {
        goto fail;
    }
    edu_dev->iomem = pcim_iomap_table(pdev)[0];

    if (param_msi) {
        // Fall back to INTx if MSI isn't available
        nvec = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES);
        if (nvec < 0) {
            err = nvec;
            goto fail;
        }
        edu_dev->irq = pci_irq_vector(pdev, 0);
    } else {
        edu_dev->irq = pdev->irq;
    }
    edu_log("irq = %u\n", edu_dev->irq);
    // need IRQF_SHARED because all (legacy) PCI IRQ lines can be shared
    err = request_irq(edu_dev->irq, edu_irq_handler, IRQF_SHARED, KBUILD_MODNAME, edu_dev);
    if (err) {
        goto fail;
    }
    edu_dev->registered_irq_handler = true;

    // add char device
    err = cdev_add(&edu_dev->cdev, devno, 1);
    if (err) {
        goto fail;
    }
    edu_dev->added_cdev = true;

    return 0;
fail:
    edu_pci_cleanup(pdev);
    return err;
}

static void edu_pci_remove(struct pci_dev *pdev) {
    pr_info("removing\n");
    edu_pci_cleanup(pdev);
}

static struct pci_driver edu_pci_driver = {
    .name = KBUILD_MODNAME,
    .id_table = edu_pci_tbl,
    .probe = edu_pci_probe,
    .remove = edu_pci_remove,
};

static void edu_driver_cleanup(void) {
    if (edu_dev) {
        kfree(edu_dev);
        edu_dev = NULL;
    }
    if (devno) {
        unregister_chrdev_region(devno, 1);
        devno = 0;
    }
}

// Uncomment __init if you don't need to debug this function in gdb
static int /* __init */ edu_init(void) {
    int err;

    // allocate device number
    err = alloc_chrdev_region(&devno, minor, 1, KBUILD_MODNAME);
    if (err) {
        return err;
    }
    // You can also get the major number by checking /proc/devices
    pr_alert("device number is %d:%d\n", MAJOR(devno), minor);
    // Now from userspace you can run e.g.
    // mknod /dev/edu c 250 0

    // initialize driver state
    edu_dev = kzalloc(sizeof(*edu_dev), GFP_KERNEL);
    if (!edu_dev) {
        err = -ENOMEM;
        goto fail;
    }
    edu_dev_init(edu_dev);

    err = pci_register_driver(&edu_pci_driver);
    if (err) {
        goto fail;
    }
    return 0;

fail:
    edu_driver_cleanup();
    return err;
}

// Uncomment __exit if you don't need to debug this function in gdb
static void /* __exit */ edu_exit(void) {
    pci_unregister_driver(&edu_pci_driver);
    edu_driver_cleanup();
}

module_init(edu_init);
module_exit(edu_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QEMU EDU device driver");
