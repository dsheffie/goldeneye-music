BUILD_DIR=build
include $(N64_INST)/include/n64.mk

OBJS = $(BUILD_DIR)/autoplay.o

# The track is baked in: there is no UI to choose one with.
TRACK ?= kamel
N64_CFLAGS += -DAUTOPLAY_TRACK=\"$(TRACK).xm64\"

all: autoplay.z64

filesystem/%.xm64: assets/%.xm
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] $@"
	@$(N64_AUDIOCONV) -o filesystem "$<"

$(BUILD_DIR)/autoplay.dfs: filesystem/$(TRACK).xm64
$(BUILD_DIR)/autoplay.elf: $(OBJS)

autoplay.z64: N64_ROM_TITLE="Autoplay"
autoplay.z64: $(BUILD_DIR)/autoplay.dfs

clean:
	rm -rf $(BUILD_DIR) filesystem autoplay.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
