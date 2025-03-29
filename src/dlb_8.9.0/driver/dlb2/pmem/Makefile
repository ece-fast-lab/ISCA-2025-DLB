#
# By default, the build is done against the running linux kernel source.
# To build against a different kernel source tree, set SYSSRC:
#
#    make SYSSRC=/path/to/kernel/source

KSRC=/lib/modules/`uname -r`/build
KOBJ=/lib/modules/`uname -r`/build

KBUILD_EXTRA_SYMBOLS += $(REPO_PATH)/src/dlb_8.9.0/driver/dlb2//Module.symvers
# KBUILD_EXTRA_SYMBOLS += /usr/src/ofa_kernel/x86_64/6.2.0-26-generic/Module.symvers
# EXTRA_CFLAGS += -I/usr/src/ofa_kernel/x86_64/6.2.0-26-generic/include/
EXTRA_CFLAGS += -I$(REPO_PATH)/src/dlb_8.9.0/driver/dlb2/

obj-m += dlb2_pmem.o
io_peer_mem-y := 

default:
	make -C $(KSRC) M=`pwd` modules

install:
	make -C $(KSRC) M=`pwd` modules_install
	depmod -a

clean:
	rm -f *.o
	rm -f *.ko
	rm -f .*.cmd
	rm -f *.mod
	rm -f dlb2_pmem.mod.c
	rm -f Module.symvers
	rm -f Module.markers
	rm -f modules.order