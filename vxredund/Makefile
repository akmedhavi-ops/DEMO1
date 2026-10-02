# vxredund - host (POSIX) build: library, tests, demo
#
#   make            build everything
#   make test       run the unit + integration test-suite
#   make clean
#   make sanitize   run the tests under ASan+UBSan and under TSan
#
# VxWorks builds: see vxworks/vx69/Makefile and vxworks/vx7/.

CC      ?= cc
CFLAGS  ?= -O2 -g
# mandatory flags kept separate so CFLAGS can be overridden (e.g. sanitizers)
RDN_CFLAGS := -std=c99 -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes \
              -Wmissing-prototypes -Wconversion -Wno-sign-conversion \
              -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc -pthread
RDN_LDFLAGS := -pthread

BUILD   := build

CORE_SRC := src/rdn_crc.c src/rdn_proto.c src/rdn_fsm.c src/rdn_repl.c \
            src/rdn_config.c src/rdn_core.c \
            links/rdn_link_loop.c links/rdn_link_udp.c links/rdn_link_vme.c \
            arbiter/rdn_arb_lease.c
OSAL_SRC := osal/rdn_osal_posix.c

LIB_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRC) $(OSAL_SRC))
LIB      := $(BUILD)/librdn.a

TESTS    := test_fsm test_proto test_repl test_vme_ring test_system
TEST_BIN := $(addprefix $(BUILD)/tests/,$(TESTS))

all: $(LIB) $(TEST_BIN) $(BUILD)/rdn_demo

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(RDN_CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/tests/%: tests/%.c tests/rdn_test.h $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(RDN_CFLAGS) -Itests $< $(LIB) $(LDFLAGS) $(RDN_LDFLAGS) -o $@

$(BUILD)/rdn_demo: demo/rdn_demo_host.c $(LIB)
	$(CC) $(CFLAGS) $(RDN_CFLAGS) $< $(LIB) $(LDFLAGS) $(RDN_LDFLAGS) -o $@

test: $(TEST_BIN)
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done
	@echo "ALL TESTS PASSED"

# Sanitizer builds are slower than the declared scheduling jitter allows,
# so latency assertions are relaxed there (functional checks still apply);
# plain `make test` is the timing-verification run.
sanitize:
	RDN_TEST_RELAXED_TIMING=1 \
	$(MAKE) BUILD=$(BUILD)/asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" test
	RDN_TEST_RELAXED_TIMING=1 TSAN_OPTIONS="suppressions=$(CURDIR)/tests/tsan.supp halt_on_error=1" \
	$(MAKE) BUILD=$(BUILD)/tsan CFLAGS="-O1 -g -fsanitize=thread" LDFLAGS="-fsanitize=thread" test

clean:
	rm -rf $(BUILD)

.PHONY: all test sanitize clean
