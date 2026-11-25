CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Werror
BUILD = build
MODEL = src/config.c src/io.c src/model.c src/tick.c
IPC = $(MODEL) src/main.c src/simulation.c src/ipc.c
TCP = src/net.c
TCP_TARGETS = airport_server airport_aircraft airport_runway airport_watch airport_ctl
IPC_TARGETS = airport airport_named airport_unnamed airport_shm airport_fifo

.PHONY: all tcp test test-tcp
all: tcp $(addprefix $(BUILD)/,$(IPC_TARGETS))
tcp: $(addprefix $(BUILD)/,$(TCP_TARGETS))

$(BUILD):
	mkdir -p $@

$(BUILD)/airport_server: src/net_server.c $(MODEL) $(TCP) src/airport.h src/net.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(filter %.c,$^) $(LDFLAGS) $(LDLIBS) -o $@

define CLIENT
$(BUILD)/airport_$(1): src/net_client.c $(TCP) src/airport.h src/net.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -DCLIENT_ROLE=$(2) $$(filter %.c,$$^) $(LDFLAGS) $(LDLIBS) -o $$@
endef
$(eval $(call CLIENT,aircraft,AIRCRAFT))
$(eval $(call CLIENT,runway,RUNWAY))
$(eval $(call CLIENT,watch,WATCH))
$(eval $(call CLIENT,ctl,CONTROL))

$(BUILD)/airport: $(IPC) src/airport.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(filter %.c,$^) $(LDFLAGS) $(LDLIBS) -pthread -lrt -o $@

$(addprefix $(BUILD)/airport_,named unnamed shm fifo): $(BUILD)/airport_%: $(IPC) src/airport.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -DDEFAULT_MODE='"$*"' $(filter %.c,$^) $(LDFLAGS) $(LDLIBS) -pthread -lrt -o $@

test-tcp: all
	python3 tests/test_tcp.py --bin-dir $(BUILD)

test: test-tcp
	AIRPORT_BIN_DIR=$(BUILD) python3 tests/run_tests.py
