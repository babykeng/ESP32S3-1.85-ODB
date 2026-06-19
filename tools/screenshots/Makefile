ROOT := ../..
OUT_DIR := out
BUILD_DIR := build
TARGET := $(BUILD_DIR)/capture_screens

CC ?= clang

CFLAGS := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS += -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE
CFLAGS += -I.
CFLAGS += -Istubs
CFLAGS += -I$(ROOT)
CFLAGS += -I$(ROOT)/components/esp32s3_obd
CFLAGS += -I$(ROOT)/components/lvgl__lvgl
CFLAGS += -I$(ROOT)/components/lvgl__lvgl/src

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
	esp_idf_stubs.c \
	capture_screens.c

SRCS := $(LVGL_SRCS) $(UI_SRCS)
ROOT_SRCS := $(filter $(ROOT)/%,$(SRCS))
LOCAL_SRCS := $(filter-out $(ROOT)/%,$(SRCS))
OBJS := $(patsubst $(ROOT)/%.c,$(BUILD_DIR)/root/%.o,$(ROOT_SRCS))
OBJS += $(patsubst %.c,$(BUILD_DIR)/local/%.o,$(LOCAL_SRCS))

.PHONY: all clean screenshots png

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

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)
