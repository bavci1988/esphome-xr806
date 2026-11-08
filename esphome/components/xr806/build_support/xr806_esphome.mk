# ESPHome XR806 integration with the Allwinner/XRadio vendor SDK.
# Included after the vendor project's gcc/Makefile.

# Root of the enclosing ESPHome repository.
XR806_REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)

# External ArduinoJson headers; repository integration follows.
XR806_ARDUINOJSON_SRC ?= $(error XR806_ARDUINOJSON_SRC must be provided by ESPHome)

XR806_OBJDIR ?= $(XR806_REPO_ROOT)/.xr806-build/obj
XR806_OUTPUT ?= $(XR806_REPO_ROOT)/.xr806-build/app.elf

# Use the ESPHome-specific C entry point instead of the SDK test main.
XR806_SDK_OBJECTS := $(filter-out $(PRJ_ROOT_PATH)/main.o,$(OBJS))

XR806_ESPHOME_OBJECTS := $(addprefix $(XR806_OBJDIR)/, \
    application.o \
    component.o \
    scheduler.o \
    helpers.o \
    platform_helpers.o \
    hal.o \
    wake_freertos.o \
    main.o \
    runtime.o \
    wlan_smoke.o)

XR806_ESPHOME_MAIN := $(XR806_OBJDIR)/sdk_main_esphome.o

# Build a separate ELF without overwriting the vendor SDK output.
$(XR806_OUTPUT): $(XR806_SDK_OBJECTS) $(XR806_ESPHOME_MAIN) $(XR806_ESPHOME_OBJECTS) | lib
	@mkdir -p $(@D)
	$(CC) -E -P -CC $(CC_SYMBOLS) \
	    -I$(ROOT_PATH)/include \
	    -I$(ROOT_PATH)/lib/xradio_v$(CONFIG_CHIP_ARCH_VER) \
	    -include generated/autoconf.h \
	    -o $(PROJECT_LD) - < $(LINKER_SCRIPT)
	$(XR806_CXX) \
	    $(LD_FLAGS) \
	    --specs=nosys.specs \
	    -T$(PROJECT_LD) \
	    $(LIBRARY_PATHS) \
	    -o $@ \
	    $(XR806_SDK_OBJECTS) \
	    $(XR806_ESPHOME_MAIN) \
	    $(XR806_ESPHOME_OBJECTS) \
	    $(LIBRARIES)

# ESPHOME_CPP_BUILD_RULES_V1
# Compile ESPHome C++20 sources using the YAML-generated configuration.

XR806_SRC ?= $(XR806_REPO_ROOT)/.xr806-build/src
XR806_DEFINES := $(XR806_SRC)/esphome/core/defines.h
XR806_CXX ?= arm-none-eabi-g++

XR806_CXXFLAGS := \
    -std=gnu++20 \
    -mcpu=cortex-m33 -mthumb \
    -mfpu=fpv5-sp-d16 -mfloat-abi=softfp -mcmse \
    -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections \
    -DUSE_NATIVE_64BIT_TIME \
    -DCONFIG_CPU_CM33F \
    -DCONFIG_CHIP_ARCH_VER=3 \
    -DCONFIG_ARCH_APP_CORE \
    -DPLATFORM_XR806=1 \
    -I$(XR806_SRC) \
    -include $(XR806_DEFINES) \
    -I$(XR806_ARDUINOJSON_SRC) \
    -I$(ROOT_PATH)/include \
    -I$(ROOT_PATH)/project \
    -I$(ROOT_PATH)/project/common/framework \
    -I$(ROOT_PATH)/include/net/lwip-2.1.2 \
    -I$(ROOT_PATH)/include/driver/cmsis \
    -I$(ROOT_PATH)/include/kernel/FreeRTOS/FreeRTOSv10.2.1 \
    -I$(ROOT_PATH)/include/kernel/FreeRTOS/FreeRTOSv10.2.1/portable/GCC/ARM_CM33_NTZ \
    -I$(ROOT_PATH)/include/kernel/FreeRTOS/FreeRTOSv10.2.1/portable/GCC/ARM_CM33_NTZ/non_secure

define XR806_COMPILE_CPP
$(XR806_OBJDIR)/$(1).o: $(2) $(XR806_DEFINES)
	@mkdir -p $$(@D)
	$$(XR806_CXX) $$(XR806_CXXFLAGS) -c $$< -o $$@
endef

$(eval $(call XR806_COMPILE_CPP,application,$(XR806_SRC)/esphome/core/application.cpp))
$(eval $(call XR806_COMPILE_CPP,component,$(XR806_SRC)/esphome/core/component.cpp))
$(eval $(call XR806_COMPILE_CPP,scheduler,$(XR806_SRC)/esphome/core/scheduler.cpp))
$(eval $(call XR806_COMPILE_CPP,helpers,$(XR806_SRC)/esphome/core/helpers.cpp))
$(eval $(call XR806_COMPILE_CPP,platform_helpers,$(XR806_SRC)/esphome/components/xr806/helpers.cpp))
$(eval $(call XR806_COMPILE_CPP,hal,$(XR806_SRC)/esphome/components/xr806/hal.cpp))
$(eval $(call XR806_COMPILE_CPP,wake_freertos,$(XR806_SRC)/esphome/core/wake/wake_freertos.cpp))
$(eval $(call XR806_COMPILE_CPP,main,$(XR806_SRC)/main.cpp))
$(eval $(call XR806_COMPILE_CPP,runtime,$(XR806_REPO_ROOT)/esphome/components/xr806/runtime.cpp))
$(eval $(call XR806_COMPILE_CPP,wlan_smoke,$(XR806_REPO_ROOT)/esphome/components/xr806/wlan_smoke.cpp))

# ESPHOME_SDK_MAIN_RULE_V1
# Build a separate SDK main.o with ESPHome enabled.
# The vendor project's original main.o remains untouched.

$(XR806_ESPHOME_MAIN): $(PRJ_ROOT_PATH)/main.c
	@mkdir -p $(@D)
	$(CC) $(CC_FLAGS) $(CC_SYMBOLS) \
	    -DTRV16_USE_ESPHOME \
	    -std=gnu99 \
	    $(INCLUDE_PATHS) \
	    -o $@ $<

# ESPHOME_CODEGEN_RULE_V1
# Regenerate ESPHome sources when YAML or platform configuration changes.

XR806_YAML ?= $(XR806_REPO_ROOT)/examples/xr806/trv16.yaml
XR806_PYTHON ?= python3
XR806_PLATFORM_PY ?= $(XR806_REPO_ROOT)/esphome/components/xr806/__init__.py

ifneq ($(XR806_SKIP_CODEGEN),1)
# Grouped targets: a single ESPHome invocation generates both files.
$(XR806_SRC)/main.cpp $(XR806_DEFINES) &: $(XR806_YAML) $(XR806_PLATFORM_PY)
	@echo "=== Generiere ESPHome-Code aus YAML ==="
	cd $(XR806_REPO_ROOT) && \
	    $(XR806_PYTHON) -m esphome compile \
	    $(XR806_YAML) --only-generate && \
	    test -s $(XR806_SRC)/main.cpp && \
	    test -s $(XR806_DEFINES) && \
	    touch $(XR806_SRC)/main.cpp $(XR806_DEFINES)
endif

# XR806_IMAGE_CONFIG_V1
# Preprocess the vendor's AWIH image configuration using SDK definitions.

XR806_IMAGE_CFG ?= $(XR806_FW_DIR)/image.cfg

$(XR806_IMAGE_CFG): $(ROOT_PATH)/project/image_cfg/image.cfg
	@mkdir -p $(@D)
	$(CC) -E -P -CC $(CC_SYMBOLS) \
	    -I$(ROOT_PATH)/include \
	    -include generated/autoconf.h \
	    -o $@ - < $<

# XR806_AWIH_PACKAGING_V1
# Build RAM/XIP binaries and package the AWIH firmware image.

XR806_FW_DIR ?= $(XR806_REPO_ROOT)/.xr806-build/firmware
XR806_AWIH ?= $(XR806_FW_DIR)/firmware.img

$(XR806_FW_DIR)/app.bin: $(XR806_OUTPUT)
	@mkdir -p $(@D)
	$(OBJCOPY) -O binary -R .xip $< $@

$(XR806_FW_DIR)/app_xip.bin: $(XR806_OUTPUT)
	@mkdir -p $(@D)
	$(OBJCOPY) -O binary -j .xip $< $@

$(XR806_FW_DIR)/mkimage: $(ROOT_PATH)/tools/mkimage
	@mkdir -p $(@D)
	install -m 0755 $< $@

$(XR806_FW_DIR)/boot_40M.bin: $(ROOT_PATH)/bin/xradio_v3/boot/xr806/boot_40M.bin
	@mkdir -p $(@D)
	install -m 0644 $< $@

$(XR806_FW_DIR)/wlan_bl.bin: $(ROOT_PATH)/bin/xradio_v3/wlan_bl.bin
	@mkdir -p $(@D)
	install -m 0644 $< $@

$(XR806_FW_DIR)/wlan_fw.bin: $(ROOT_PATH)/bin/xradio_v3/wlan_fw.bin
	@mkdir -p $(@D)
	install -m 0644 $< $@

$(XR806_FW_DIR)/sys_sdd_40M.bin: $(ROOT_PATH)/bin/xradio_v3/sys_sdd_40M.bin
	@mkdir -p $(@D)
	install -m 0644 $< $@

$(XR806_AWIH): $(XR806_FW_DIR)/app.bin $(XR806_FW_DIR)/app_xip.bin $(XR806_IMAGE_CFG) $(XR806_FW_DIR)/mkimage $(XR806_FW_DIR)/boot_40M.bin $(XR806_FW_DIR)/wlan_bl.bin $(XR806_FW_DIR)/wlan_fw.bin $(XR806_FW_DIR)/sys_sdd_40M.bin
	cd $(XR806_FW_DIR) && ./mkimage -c image.cfg -o $(notdir $@)

# XR806_GPIO_BUILD_RULE_V2
# Additional GPIO object, independent of existing object-list formatting.
XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/gpio.o

# Explicit prerequisite ensures the ELF is relinked when gpio.o changes.
$(XR806_OUTPUT): $(XR806_OBJDIR)/gpio.o

$(eval $(call XR806_COMPILE_CPP,gpio,$(XR806_SRC)/esphome/components/xr806/gpio.cpp))


# XR806_BINARY_SENSOR_BUILD_V1
# ESPHome entity and GPIO binary-sensor implementations.

# === XR806_ENTITY_BASE_V2 ===
# Shared core entity implementation.

XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/entity_base.o
$(XR806_OUTPUT): $(XR806_OBJDIR)/entity_base.o

$(eval $(call XR806_COMPILE_CPP,entity_base,$(XR806_SRC)/esphome/core/entity_base.cpp))

# === END XR806_ENTITY_BASE_V2 ===

# XR806_CORE_GPIO_BUILD_V1
# ESPHome core GPIO logging and utilities.
# core_gpio.o avoids collision with XR806 platform gpio.o.

XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/core_gpio.o

$(XR806_OUTPUT): $(XR806_OBJDIR)/core_gpio.o

$(eval $(call XR806_COMPILE_CPP,core_gpio,$(XR806_SRC)/esphome/core/gpio.cpp))


# XR806_CORE_LOG_BUILD_V1
# ESPHome core logging implementation.

XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/core_log.o

$(XR806_OUTPUT): $(XR806_OBJDIR)/core_log.o

$(eval $(call XR806_COMPILE_CPP,core_log,$(XR806_SRC)/esphome/core/log.cpp))

# === XR806_AUTO_COMPONENTS_V1 ===
# Discover YAML-generated ESPHome component implementations.
# XR806 platform files are built by their dedicated rules.
# Core files remain explicitly selected to exclude foreign
# platform implementations such as wake_esp8266.cpp.

XR806_AUTO_COMPONENT_SRCS := $(shell find $(XR806_SRC)/esphome/components -type f -name '*.cpp' ! -path '$(XR806_SRC)/esphome/components/xr806/*' 2>/dev/null | LC_ALL=C sort)

XR806_AUTO_COMPONENT_OBJECTS := $(patsubst $(XR806_SRC)/esphome/components/%.cpp,$(XR806_OBJDIR)/auto/components/%.o,$(XR806_AUTO_COMPONENT_SRCS))

XR806_ESPHOME_OBJECTS += $(XR806_AUTO_COMPONENT_OBJECTS)

$(XR806_OUTPUT): $(XR806_AUTO_COMPONENT_OBJECTS)

$(XR806_OBJDIR)/auto/components/%.o: $(XR806_SRC)/esphome/components/%.cpp $(XR806_DEFINES)
	@mkdir -p $(@D)
	$(XR806_CXX) $(XR806_CXXFLAGS) -c $< -o $@

# === END XR806_AUTO_COMPONENTS_V1 ===


# === XR806_PREFERENCES_V1 ===
# Non-persistent preferences manager for early XR806 bring-up.

XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/xr806_preferences.o
$(XR806_OUTPUT): $(XR806_OBJDIR)/xr806_preferences.o

$(eval $(call XR806_COMPILE_CPP,xr806_preferences,$(XR806_REPO_ROOT)/esphome/components/xr806/preferences.cpp))

# === END XR806_PREFERENCES_V1 ===
