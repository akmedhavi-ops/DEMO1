# VxWorks 7 layer build (TEMPLATE) - builds libvxredund.a in the VSB.
# Verify include-file names against your VxWorks 7 release.

LIB_BASE_NAME = vxredund

SRC_TOP = ../..

EXTRA_INCLUDE += -I$(SRC_TOP)/include -I$(SRC_TOP)/src

OBJS = rdn_crc.o rdn_proto.o rdn_fsm.o rdn_repl.o rdn_config.o rdn_core.o \
       rdn_osal_vxworks.o rdn_link_udp.o rdn_link_vme.o rdn_link_loop.o \
       rdn_arb_lease.o

vpath %.c $(SRC_TOP)/src $(SRC_TOP)/osal $(SRC_TOP)/links $(SRC_TOP)/arbiter

include $(WIND_KRNL_MK)/rules.library.mk
