obj-m += bce.o
bce-objs := pci.o mailbox.o queue.o queue_dma.o vhci/vhci.o vhci/queue.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean