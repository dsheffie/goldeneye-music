BUILD_DIR=build
include $(N64_INST)/include/n64.mk
OBJS = $(BUILD_DIR)/supersaw.o
all: supersaw.z64

filesystem/%.wav64: assets/%.wav
	@mkdir -p $(dir $@)
	@echo "    [AUDIO] $@"
	@$(N64_AUDIOCONV) -o filesystem "$<"

$(BUILD_DIR)/supersaw.dfs: filesystem/saw.wav64 filesystem/soft.wav64 filesystem/kick.wav64
$(BUILD_DIR)/supersaw.elf: $(OBJS)
supersaw.z64: N64_ROM_TITLE="Supersaw"
supersaw.z64: $(BUILD_DIR)/supersaw.dfs
clean:
	rm -rf $(BUILD_DIR) filesystem supersaw.z64
-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
