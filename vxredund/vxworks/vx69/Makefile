# vxredund - VxWorks 6.9 downloadable kernel module (DKM), MVME5500 (MPC7457)
#
# From a VxWorks development shell (wrenv -p vxworks-6.9):
#     make                      -> vxredund.out
#     make TIMEBASE=1           use the PowerPC time base as clock
# On the target shell:
#     ld < vxredund.out
#     rdnDemoStart 1, "192.168.100.1", "192.168.100.2", 0, 0x08000000
#
# Alternatively add the same sources to a Workbench DKM project, or to the
# kernel image (VIP) and call rdnDemoStart() from usrAppInit().

CPU      ?= PPC32
TOOL     ?= gnu
CC       := ccppc
LD       := ldppc
TGT      := $(WIND_BASE)/target
TOP      := ../..

# -mlongcall: DKMs can be loaded anywhere in the 32-bit space
# -mstrict-align: 74xx alignment faults on cache-inhibited (VME) space
CFLAGS   := -mcpu=7450 -mstrict-align -mlongcall -fno-builtin -O2 \
            -std=gnu99 -Wall -Wextra \
            -D_WRS_KERNEL -DCPU=$(CPU) -DTOOL_FAMILY=gnu -DTOOL=$(TOOL) \
            -I$(TGT)/h -I$(TGT)/h/wrn/coreip \
            -I$(TOP)/include -I$(TOP)/src
ifeq ($(TIMEBASE),1)
CFLAGS   += -DRDN_VX_USE_TIMEBASE
endif

SRC := $(TOP)/src/rdn_crc.c $(TOP)/src/rdn_proto.c $(TOP)/src/rdn_fsm.c \
       $(TOP)/src/rdn_repl.c $(TOP)/src/rdn_config.c $(TOP)/src/rdn_core.c \
       $(TOP)/osal/rdn_osal_vxworks.c \
       $(TOP)/links/rdn_link_udp.c $(TOP)/links/rdn_link_vme.c \
       $(TOP)/links/rdn_link_loop.c $(TOP)/arbiter/rdn_arb_lease.c \
       $(TOP)/vxworks/rdn_vx_mvme5500.c
OBJ := $(patsubst $(TOP)/%.c,obj/%.o,$(SRC))

vxredund.out: $(OBJ)
	$(LD) -r -o $@ $^

obj/%.o: $(TOP)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf obj vxredund.out

.PHONY: clean
