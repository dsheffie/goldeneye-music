BUILD_DIR=build
include $(N64_INST)/include/n64.mk
OBJS = $(BUILD_DIR)/vadpcm.o
all: vadpcm.z64

filesystem/%.wav64: assets/%.wav
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] $@"
	@$(N64_AUDIOCONV) --wav-compress 1 -o filesystem "$<"

$(BUILD_DIR)/vadpcm.dfs: filesystem/monosample8.wav64
$(BUILD_DIR)/vadpcm.elf: $(OBJS)
vadpcm.z64: N64_ROM_TITLE="VADPCM"
vadpcm.z64: $(BUILD_DIR)/vadpcm.dfs
clean:
	rm -rf $(BUILD_DIR) filesystem vadpcm.z64
-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
