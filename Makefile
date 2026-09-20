PROJECT_ROOT := $(abspath .)
BUILD_DIR := build
ROM := $(BUILD_DIR)/WastesEdge.v32

BUILD_ROM := $(PROJECT_ROOT)/scripts/build-rom.sh

.PHONY: all clean

all: $(ROM)

$(ROM): main.c $(BUILD_ROM) $(WASM2VIRCON) \
        $(PROJECT_ROOT)/include/vircon.h \
        $(PROJECT_ROOT)/runtime/vircon_runtime.c \
        $(PROJECT_ROOT)/runtime/vircon_text.c \
        $(PROJECT_ROOT)/runtime/vircon_platform.h
	"$(BUILD_ROM)" --normalize \
	  --entry __original_main \
	  --include "$(PROJECT_ROOT)/include" \
	  --include "$(PROJECT_ROOT)/runtime" \
	  --extra-source "$(PROJECT_ROOT)/runtime/vircon_runtime.c" \
	  --extra-source "$(PROJECT_ROOT)/runtime/vircon_text.c" \
	  main.c "$(BUILD_DIR)"

clean:
	rm -rf "$(BUILD_DIR)"
