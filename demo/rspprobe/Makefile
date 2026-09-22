BUILD_DIR=build
include $(N64_INST)/include/n64.mk

OBJS = $(BUILD_DIR)/probe.o

all: rspprobe.z64

$(BUILD_DIR)/probe.o: $(BUILD_DIR)/rsp_probe.o
$(BUILD_DIR)/rspprobe.elf: $(OBJS) $(BUILD_DIR)/rsp_probe.o

rspprobe.z64: N64_ROM_TITLE="RSP Probe"

clean:
	rm -rf $(BUILD_DIR) rspprobe.z64
-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean
