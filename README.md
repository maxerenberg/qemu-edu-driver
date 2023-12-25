# qemu-edu-driver
This is a Linux kernel module written for the [QEMU edu device](https://www.qemu.org/docs/master/specs/edu.html).
I found it useful to write a driver for this device while reading
[Linux Device Drivers, Third Edition](https://lwn.net/Kernel/LDD3/), because
it allowed me to put the concepts into practice.

## Building the module
On Debian-based systems, install the `linux-headers-amd64` package, then run:
```bash
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
```

Alternatively, if you cloned the [kernel repository](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git)
and already have a custom .config, then you can just point the `make` command
to that directory instead:
```bash
make -C /path/to/your/linux/tree M=$(pwd) modules
```

Make sure that the kernel version for which you are building the module is
exactly the same as the kernel version running in the VM. If you already
built a custom kernel, you can pass it to QEMU using the `-kernel` option.

## Loading the module
Copy the `edu.ko` file into the VM, then run
```bash
insmod edu.ko debug=1 msi=1
```
The following parameters are supported:
* debug (bool): if true, verbose log messages will be enabled
* msi (bool): if true, MSI will be used instead of the legacy pin-based interrupt

In the dmesg output, you should see a message which looks like this:
```
edu:edu_init: device number is 250:0
```

Alternatively, you can grep for "edu" in /proc/devices. Once you have the major/minor
device numbers, you can create a device node:
```bash
mknod /dev/edu c 250 0
```
The device node must exist to use edu-cli (see below).

## edu-cli
This repository has a CLI program called edu-cli which can be used to interact with
the driver via an IOCTL-based interface. To build it, run
```bash
make edu-cli
```

It supports the following commands:
* `edu-cli ident`: Show the major/minor version of the edu device
* `edu-cli liveness <number>`: Perform a "liveness check". This just writes a
  number to the device, and the device computes the bitwise NOT of the value.
* `edu-cli factorial <number>`: Compute a factorial. This uses interrupts to
  wait for the calculation to finish.
* `edu-cli wait`: Wait for any interrupt to arrive, and print the value in the
  interrupt status register.
* `edu-cli raise <number>`: Raise an interrupt. The number will appear in the
  interrupt status register.
* `edu-cli dma-write`: Write a string into EDU device buffer via DMA. The
  string is accepted over stdin and can be at most 4096 bytes. Example:
  `echo hello | ./edu-cli dma-write`
* `edu-cli dma-read <size>`: Read `size` bytes from the EDU device buffer via
  DMA. The bytes are printed to stdout.
