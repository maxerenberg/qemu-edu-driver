obj-m := edu.o
edu-objs := edu-driver.o

edu-cli: edu-cli.c
	$(CC) -Wall -Wextra -std=gnu11 -o $@ $<

.PHONY: clean
clean:
	$(RM) edu-cli *.o *.ko modules.order .modules.order.cmd Module.symvers .Module.symvers.cmd .*.ko.cmd *.mod *.mod.c .*.mod.cmd *.mod.o .*.o.cmd
