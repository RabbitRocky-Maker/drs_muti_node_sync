CC      := $(CROSS)gcc

# ── Runtime tunables (override on command line) ──────────────────────────────
# Network interface the daemon listens on
IFACE    ?= eth0
# UDP telemetry destination IP (port 4242); 127.0.0.1 = local only
TELEM_IP ?= 127.0.0.1
# Protocol wire-frame version:
#   PROTO_VER=1  →  66 bytes (12 bytes padding)
#   PROTO_VER=2  →  64 bytes (10 bytes padding)  [default]
PROTO_VER ?= 2

CFLAGS  := -O2 -g -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE \
           -I include -DDRS_PROTO_VER=$(PROTO_VER)
LDFLAGS := -lpthread -lrt

SRCDIR  := src
TESTDIR := tests
TOOLDIR := tools
BUILDDIR := build

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))

# Exclude main.c from the object list used by tests/tools
LIB_OBJS := $(filter-out $(BUILDDIR)/main.o, $(OBJS))

TESTS := test_crc32 test_proto test_min_delay test_pi_ctrl test_vclock
TEST_BINS := $(addprefix $(BUILDDIR)/, $(TESTS))

.PHONY: all clean test install

all: $(BUILDDIR)/drs_syncd $(BUILDDIR)/drs_sync_inspect $(BUILDDIR)/drs_mon

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/drs_syncd: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ── Inspect tool ────────────────────────────────────────────────────────
$(BUILDDIR)/drs_sync_inspect: $(TOOLDIR)/drs_sync_inspect.c $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ── Live monitor ─────────────────────────────────────────────────────────
$(BUILDDIR)/drs_mon: $(TOOLDIR)/drs_mon.c $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ── Unit tests ──────────────────────────────────────────────────────────
$(BUILDDIR)/test_%: $(TESTDIR)/test_%.c $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: $(TEST_BINS)
	@echo "=== Running unit tests ==="
	@for t in $(TEST_BINS); do \
		echo "--- $$t ---"; \
		$$t; \
	done
	@echo "=== All tests done ==="

clean: uninstall-systemd
	rm -rf $(BUILDDIR)

install: $(BUILDDIR)/drs_syncd $(BUILDDIR)/drs_sync_inspect $(BUILDDIR)/drs_mon
	install -m 755 $(BUILDDIR)/drs_syncd /usr/local/bin/
	install -m 755 $(BUILDDIR)/drs_sync_inspect /usr/local/bin/
	install -m 755 $(BUILDDIR)/drs_mon /usr/local/bin/

# systemd units (requires root — use: sudo make install-systemd)
# Override IFACE and TELEM_IP on the command line:
#   sudo make install-systemd IFACE=eth0 TELEM_IP=192.168.1.50
install-systemd: install
	@if [ "$$(id -u)" -ne 0 ]; then echo "ERROR: must be root (use sudo) to install systemd units"; exit 1; fi
	@echo "[drs] installing systemd units  IFACE=$(IFACE)  TELEM_IP=$(TELEM_IP)"
	# Generate drs-sync.service with runtime arguments baked in
	sed 's|ExecStart=.*|ExecStart=/usr/local/bin/drs_syncd $(IFACE) auto $(TELEM_IP)|' \
	    deploy/drs-sync.service > /tmp/drs-sync-configured.service
	install -m 644 /tmp/drs-sync-configured.service /etc/systemd/system/drs-sync.service
	rm -f /tmp/drs-sync-configured.service
	# Static dependency services
	install -m 644 deploy/drs-sync-precond.service   /etc/systemd/system/
	install -m 644 deploy/drs-perf-governor.service  /etc/systemd/system/
	install -m 644 deploy/drs-irq-affinity.service   /etc/systemd/system/
	install -m 644 deploy/drs-nic-tune.service       /etc/systemd/system/
	systemctl daemon-reload
	# Enable all units so they survive reboot
	systemctl enable drs-sync-precond.service \
	                 drs-perf-governor.service \
	                 drs-irq-affinity.service \
	                 drs-nic-tune.service \
	                 drs-sync.service
	# Start the full stack right now (preconditions first, then daemon)
	systemctl restart drs-sync-precond.service  || true
	systemctl restart drs-perf-governor.service || true
	systemctl restart drs-irq-affinity.service  || true
	systemctl restart drs-nic-tune.service      || true
	systemctl restart drs-sync.service
	@echo "[drs] installation complete — use 'systemctl status drs-sync.service' to verify"

uninstall-systemd:
	@if [ "$$(id -u)" -ne 0 ]; then echo "WARNING: not root — skipping systemd unit removal (run 'sudo make uninstall-systemd' to clean)"; exit 0; fi
	systemctl stop    drs-sync.service 2>/dev/null || true
	systemctl disable drs-sync.service \
	                  drs-sync-precond.service \
	                  drs-perf-governor.service \
	                  drs-irq-affinity.service \
	                  drs-nic-tune.service 2>/dev/null || true
	rm -f /etc/systemd/system/drs-sync.service
	rm -f /etc/systemd/system/drs-sync-precond.service
	rm -f /etc/systemd/system/drs-perf-governor.service
	rm -f /etc/systemd/system/drs-irq-affinity.service
	rm -f /etc/systemd/system/drs-nic-tune.service
	systemctl daemon-reload 2>/dev/null || true
