ifneq ($(VERSION).$(PATCHLEVEL),2.4)

# Makefile frag for Linux v2.6 and v3.x

ifeq ($(CONFIG_X86_64),y)
X86_MODE := 64
else
X86_MODE := 32
endif

obj-$(CONFIG_XENOMAI) += xeno_hal.o

xeno_hal-y := hal_$(X86_MODE).o hal-common.o usercopy_$(X86_MODE).o smi.o

EXTRA_CFLAGS += -D__IN_XENOMAI__ -Iinclude/xenomai

else

# Makefile frag for Linux v2.4

USE_STANDARD_AS_RULE := true

O_TARGET := built-in.o

obj-y := hal_32.o hal-common.o smi.o

export-objs := $(obj-y) $(obj-m)

EXTRA_CFLAGS += -D__IN_XENOMAI__ -I$(TOPDIR)/include/xenomai -I$(TOPDIR)/include/xenomai/compat

include $(TOPDIR)/Rules.make

endif
