PROJECT_ROOT := $(abspath .)
BUILD_DIR := build
ROM_NAME := WastesEdge.v32

ROM_PATH := $(BUILD_DIR)/$(ROM_NAME)
BUILD_ROM := $(PROJECT_ROOT)/scripts/build-rom.sh
CONTENT_COMPILER := $(PROJECT_ROOT)/tools/content_compiler/compile_content.py
CONTENT_INPUTS := $(shell find gamedata -type f \( -name '*.jsonc' -o -name '*.tmj' -o -name '*.tsj' -o -name '*.png' -o -name '*.ttf' -o -name '*.wav' -o -name '*.ogg' \) 2>/dev/null | sort)
GENERATED_CONTENT := $(PROJECT_ROOT)/generated/content_data.c
GENERATED_IDS := $(PROJECT_ROOT)/generated/content_ids.h
GENERATED_SCENE_DATA := $(PROJECT_ROOT)/generated/content_scene_data.c
GENERATED_SCENE_HEADER := $(PROJECT_ROOT)/generated/content_scene_data.h
GENERATED_ASSETS := $(PROJECT_ROOT)/generated/content_assets.mk
GENERATED_OUTPUTS := $(GENERATED_CONTENT) $(GENERATED_IDS) $(GENERATED_SCENE_DATA) $(GENERATED_SCENE_HEADER) $(GENERATED_ASSETS)
GENERATED_STAMP := $(PROJECT_ROOT)/generated/.content-stamp
RUNTIME_SOURCES := $(PROJECT_ROOT)/src/engine/world.c $(PROJECT_ROOT)/src/platform/vircon32/platform.c

-include $(GENERATED_ASSETS)
TEXTURE_ARGS := $(foreach texture,$(CONTENT_TEXTURES),--texture "$(PROJECT_ROOT)/gamedata/$(texture)")
TEXTURE_ARGS += $(foreach texture,$(CONTENT_GENERATED_TEXTURES),--texture "$(PROJECT_ROOT)/$(texture)")
GENERATED_TEXTURE_FILES := $(addprefix $(PROJECT_ROOT)/,$(CONTENT_GENERATED_TEXTURES))

VIRCON32_EMULATOR ?= /usr/local/Vircon32/Emulator/Vircon32

.PHONY: all run clean

all: $(ROM_PATH)

$(ROM_PATH): main.c $(RUNTIME_SOURCES) $(BUILD_ROM) $(WASM2VIRCON) $(GENERATED_CONTENT) $(GENERATED_IDS) \
	$(GENERATED_SCENE_DATA) $(GENERATED_SCENE_HEADER) $(PROJECT_ROOT)/include/vircon.h $(GENERATED_TEXTURE_FILES)
	"$(BUILD_ROM)" \
	  --entry __original_main \
	  --rom-name "$(ROM_NAME)" \
	  --include "$(PROJECT_ROOT)/include" \
	  --include "$(PROJECT_ROOT)/src" \
	  --include "$(PROJECT_ROOT)/generated" \
	  --extra-source "$(GENERATED_CONTENT)" \
	  --extra-source "$(GENERATED_SCENE_DATA)" \
	  --extra-source "$(PROJECT_ROOT)/src/engine/world.c" \
	  --extra-source "$(PROJECT_ROOT)/src/platform/vircon32/platform.c" \
	  $(TEXTURE_ARGS) \
	  main.c "$(BUILD_DIR)"

# Derived background chunks are regenerated from Tiled data if one is missing.
$(GENERATED_TEXTURE_FILES):
	python3 "$(CONTENT_COMPILER)" --project-root "$(PROJECT_ROOT)"

$(GENERATED_OUTPUTS): $(GENERATED_STAMP)
	@:

$(GENERATED_STAMP): $(CONTENT_COMPILER) $(CONTENT_INPUTS)
	python3 "$(CONTENT_COMPILER)" --project-root "$(PROJECT_ROOT)"
	touch "$(GENERATED_STAMP)"

run: $(ROM_PATH)
	"$(VIRCON32_EMULATOR)" "$(ROM_PATH)"

clean:
	rm -rf "$(BUILD_DIR)"
