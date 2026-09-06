KDIR ?= $(HOME)/kernels/linux-5.10.29
PWD := $(shell pwd)
MODULE := a2-freezer-symbols

obj-m += $(MODULE).o

.PHONY: all probe clean

all: probe

probe:
	@set -eu; \
	log=$$(mktemp); \
	trap 'rm -f "$$log"' EXIT HUP INT TERM; \
	if $(MAKE) -C "$(KDIR)" M="$(PWD)" modules >"$$log" 2>&1; then \
		cat "$$log"; \
		echo "A2_FREEZER: COMPILE_PASS"; \
		echo "A2_FREEZER: SYMBOLS criu_cgroup_freeze_threadgroup criu_cgroup_thaw_threadgroup"; \
		echo "A2_FREEZER: PASS"; \
	else \
		cat "$$log"; \
		if grep -Eq 'undefined!|Unknown symbol|ERROR: modpost' "$$log"; then \
			echo "A2_FREEZER: COMPILE_PASS"; \
			echo "A2_FREEZER: MODPOST_UNRESOLVED"; \
			echo "A2_FREEZER: BLOCKED"; \
			exit 2; \
		fi; \
		echo "A2_FREEZER: COMPILE_FAILED"; \
		echo "A2_FREEZER: BLOCKED"; \
		exit 1; \
	fi

clean:
	$(MAKE) -C "$(KDIR)" M="$(PWD)" clean
