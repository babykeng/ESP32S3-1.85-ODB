ROOT := ../..
OUT_DIR := out
BUILD_DIR := build
TARGET := $(BUILD_DIR)/capture_screens

CC ?= clang

CFLAGS := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS += -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE
CFLAGS += -Dtime=screenshot_time -Dgettimeofday=screenshot_gettimeofday
CFLAGS += -I.
CFLAGS += -Istubs
CFLAGS += -I$(ROOT)
CFLAGS += -I$(ROOT)/components/esp32s3_obd
CFLAGS += -I$(ROOT)/components/lvgl__lvgl
CFLAGS += -I$(ROOT)/components/lvgl__lvgl/src
CFLAGS += $(EXTRA_CFLAGS)

LDLIBS := -lm

LVGL_SRCS := $(shell find $(ROOT)/components/lvgl__lvgl/src/core \
	$(ROOT)/components/lvgl__lvgl/src/hal \
	$(ROOT)/components/lvgl__lvgl/src/misc \
	$(ROOT)/components/lvgl__lvgl/src/widgets \
	$(ROOT)/components/lvgl__lvgl/src/font \
	$(ROOT)/components/lvgl__lvgl/src/extra/layouts \
	$(ROOT)/components/lvgl__lvgl/src/extra/themes \
	$(ROOT)/components/lvgl__lvgl/src/extra/others/snapshot \
	-name '*.c')
LVGL_SRCS += $(ROOT)/components/lvgl__lvgl/src/extra/lv_extra.c
LVGL_SRCS += $(shell find $(ROOT)/components/lvgl__lvgl/src/draw -maxdepth 1 -name '*.c')
LVGL_SRCS += $(shell find $(ROOT)/components/lvgl__lvgl/src/draw/sw -name '*.c')

UI_SRCS := \
	$(ROOT)/components/esp32s3_obd/ui.c \
	$(ROOT)/components/esp32s3_obd/screens/ui_ScreenPageLogo.c \
	$(ROOT)/components/esp32s3_obd/screens/ui_ScreenPageClock.c \
	$(ROOT)/components/esp32s3_obd/screens/ui_ScreenPageIMU.c \
	$(ROOT)/components/esp32s3_obd/screens/ui_ScreenPageDashboard.c \
	$(ROOT)/components/esp32s3_obd/screens/ui_ScreenPageUART.c \
	$(ROOT)/components/esp32s3_obd/screens/ui_ScreenPageDetails.c \
	$(ROOT)/components/esp32s3_obd/screens/ui_ScreenPageBluetooth.c \
	$(ROOT)/components/esp32s3_obd/fonts/ui_font_FontTypoderSize20.c \
	$(ROOT)/components/esp32s3_obd/fonts/ui_font_FontTypoderSize24.c \
	$(ROOT)/components/esp32s3_obd/fonts/ui_font_FontTypoderSize28.c \
	$(ROOT)/components/esp32s3_obd/fonts/ui_font_FontTypoderSize100.c \
	$(ROOT)/components/esp32s3_obd/fonts/ui_font_FontTypoderSize140.c \
	$(ROOT)/components/esp32s3_obd/images/pngLogoSkyGarage.c \
	$(wildcard $(ROOT)/components/esp32s3_obd/images/clockStaticBg.c) \
	$(wildcard $(ROOT)/components/esp32s3_obd/images/imuStaticBg.c) \
	esp_idf_stubs.c \
	capture_screens.c

SRCS := $(LVGL_SRCS) $(UI_SRCS)
ROOT_SRCS := $(filter $(ROOT)/%,$(SRCS))
LOCAL_SRCS := $(filter-out $(ROOT)/%,$(SRCS))
OBJS := $(patsubst $(ROOT)/%.c,$(BUILD_DIR)/root/%.o,$(ROOT_SRCS))
OBJS += $(patsubst %.c,$(BUILD_DIR)/local/%.o,$(LOCAL_SRCS))

.PHONY: all clean screenshots png fixed-clock-golden fixed-clock-current compare-clock fixed-imu-golden fixed-imu-current compare-imu

all: screenshots

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/root/%.o: $(ROOT)/%.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/local/%.o: %.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDLIBS) -o $@

screenshots: $(TARGET)
	mkdir -p $(OUT_DIR)
	$(TARGET) $(OUT_DIR)
	@for f in $(OUT_DIR)/*.ppm; do \
		png="$${f%.ppm}.png"; \
		sips -s format png "$$f" --out "$$png" >/dev/null; \
		rm "$$f"; \
	done
	@echo "screenshots written to tools/screenshots/$(OUT_DIR)"

fixed-clock-golden: $(TARGET)
	mkdir -p $(OUT_DIR)
	SCREENSHOT_FIXED_EPOCH=1766160000 $(TARGET) $(OUT_DIR)
	cp $(OUT_DIR)/02_clock.ppm $(OUT_DIR)/02_clock_golden.ppm
	@rm -f $(OUT_DIR)/0[134567]_*.ppm $(OUT_DIR)/02_clock.ppm

fixed-clock-current: $(TARGET)
	mkdir -p $(OUT_DIR)
	SCREENSHOT_FIXED_EPOCH=1766160000 $(TARGET) $(OUT_DIR)
	cp $(OUT_DIR)/02_clock.ppm $(OUT_DIR)/02_clock_current.ppm
	@rm -f $(OUT_DIR)/0[134567]_*.ppm $(OUT_DIR)/02_clock.ppm

compare-clock:
	cmp -s $(OUT_DIR)/02_clock_golden.ppm $(OUT_DIR)/02_clock_current.ppm
	@echo "clock screenshot pixel match: 0 diff"

fixed-imu-golden: $(TARGET)
	mkdir -p $(OUT_DIR)
	SCREENSHOT_FIXED_EPOCH=1766160000 $(TARGET) $(OUT_DIR)
	cp $(OUT_DIR)/03_imu.ppm $(OUT_DIR)/03_imu_golden.ppm
	@rm -f $(OUT_DIR)/0[124567]_*.ppm $(OUT_DIR)/03_imu.ppm

fixed-imu-current: $(TARGET)
	mkdir -p $(OUT_DIR)
	SCREENSHOT_FIXED_EPOCH=1766160000 $(TARGET) $(OUT_DIR)
	cp $(OUT_DIR)/03_imu.ppm $(OUT_DIR)/03_imu_current.ppm
	@rm -f $(OUT_DIR)/0[124567]_*.ppm $(OUT_DIR)/03_imu.ppm

compare-imu:
	cmp -s $(OUT_DIR)/03_imu_golden.ppm $(OUT_DIR)/03_imu_current.ppm
	@echo "imu screenshot pixel match: 0 diff"

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
