CONFIG_MODULE_FORCE_UNLOAD=y

EXTRA_CFLAGS=-Wall -Wmissing-prototypes -Wstrict-prototypes -g -O2

obj-m += snd-minivosc.o

snd-minivosc-objs  := minivosc.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

user:
	gcc genetlink-client.c -Wall `pkg-config --libs --cflags libnl-genl-3.0`

insmod:
	sudo insmod ./snd-minivosc.ko

rmmod:
	sudo rmmod ./snd-minivosc.ko
