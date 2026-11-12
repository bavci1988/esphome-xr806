# ESPHome XR806 integration with the Allwinner/XRadio vendor SDK.
# Included after the vendor project's gcc/Makefile.

# Recipe lines in this file start with '>' instead of a tab (the project
# forbids tab characters). Reset to the default at the end of the file.
.RECIPEPREFIX := >

# Root of the ESPHome source tree (the directory that contains esphome/).
XR806_REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)

# Set by run_compile() in esphome/components/xr806/__init__.py.
XR806_ARDUINOJSON_SRC ?= $(error XR806_ARDUINOJSON_SRC must be provided by ESPHome)

XR806_OBJDIR ?= $(error XR806_OBJDIR must be provided by ESPHome)
XR806_OUTPUT ?= $(error XR806_OUTPUT must be provided by ESPHome)

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
    xr_wifi_standby.o \
    xr_api_standby.o \
    xr_mqtt_standby.o \
    xr_http_standby.o \
    xr_socket_compat.o \
    xr_socket_core.o \
    xr_socket_lwip.o)

XR806_ESPHOME_MAIN := $(XR806_OBJDIR)/sdk_main_esphome.o

# Build a separate ELF without overwriting the vendor SDK output.
$(XR806_OUTPUT): $(XR806_SDK_OBJECTS) $(XR806_ESPHOME_MAIN) $(XR806_ESPHOME_OBJECTS) | lib
>@mkdir -p $(@D)
>$(CC) -E -P -CC $(CC_SYMBOLS) \
>    -I$(ROOT_PATH)/include \
>    -I$(ROOT_PATH)/lib/xradio_v$(CONFIG_CHIP_ARCH_VER) \
>    -include generated/autoconf.h \
>    -o $(PROJECT_LD) - < $(LINKER_SCRIPT)
>$(XR806_CXX) \
>    $(LD_FLAGS) \
>    --specs=nosys.specs \
>    -T$(PROJECT_LD) \
>    $(LIBRARY_PATHS) \
>    -o $@ \
>    $(XR806_SDK_OBJECTS) \
>    $(XR806_ESPHOME_MAIN) \
>    $(XR806_ESPHOME_OBJECTS) \
>    $(LIBRARIES)

# Weakened copy of the WLAN PHY library from the SDK project Makefile.
ifdef TRV16_PHY_LIB
$(XR806_OUTPUT): $(TRV16_PHY_LIB)
endif

# Compile ESPHome C++20 sources using the YAML-generated configuration.

XR806_SRC ?= $(error XR806_SRC must be provided by ESPHome)
XR806_DEFINES := $(XR806_SRC)/esphome/core/defines.h
# Compiler flags from cg.add_build_flag(), such as the log level. The stamp file changes
# only when they do, so a changed flag rebuilds every object that it affects.
XR806_EXTRA_CXXFLAGS ?=
XR806_FLAGS_STAMP ?=
XR806_DEPS := $(XR806_DEFINES) $(XR806_FLAGS_STAMP)
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
    -I$(XR806_REPO_ROOT) \
    -include $(XR806_DEFINES) \
    -include $(XR806_SRC)/esphome/components/xr806/socket_compat.h \
    -include $(XR806_SRC)/esphome/components/xr806/api_compat.h \
    -I$(XR806_ARDUINOJSON_SRC) \
    -I$(ROOT_PATH)/include \
    -I$(ROOT_PATH)/project \
    -I$(ROOT_PATH)/project/common/framework \
    -I$(ROOT_PATH)/include/net/lwip-2.1.2 \
    -I$(ROOT_PATH)/include/net/mbedtls-2.16.0 \
    -I$(ROOT_PATH)/include/net/mbedtls-2.16.0/mbedtls/configs \
    "-DMBEDTLS_CONFIG_FILE=<config-xr-mini-cliserv.h>" \
    -I$(ROOT_PATH)/include/driver/cmsis \
    -I$(ROOT_PATH)/include/kernel/FreeRTOS/FreeRTOSv10.2.1 \
    -I$(ROOT_PATH)/include/kernel/FreeRTOS/FreeRTOSv10.2.1/portable/GCC/ARM_CM33_NTZ \
    -I$(ROOT_PATH)/include/kernel/FreeRTOS/FreeRTOSv10.2.1/portable/GCC/ARM_CM33_NTZ/non_secure \
    $(XR806_EXTRA_CXXFLAGS)

# Archives supplied by ESPHome run_compile.
ifneq ($(strip $(XR806_NOISE_C_ARCHIVE)),)
XR806_CXXFLAGS += \
    -I$(XR806_NOISE_C_INCLUDE) \
    -I$(XR806_NOISE_SRC_INCLUDE) \
    -I$(XR806_SODIUM_INCLUDE) \
    -I$(XR806_SODIUM_PORT_INCLUDE)
$(XR806_OUTPUT): $(XR806_NOISE_C_ARCHIVE) $(XR806_SODIUM_ARCHIVE)
LIBRARIES += $(XR806_NOISE_C_ARCHIVE) $(XR806_SODIUM_ARCHIVE) -lm -lc -lgcc
endif

define XR806_COMPILE_CPP
$(XR806_OBJDIR)/$(1).o: $(2) $(XR806_DEPS)
>@mkdir -p $$(@D)
>$$(XR806_CXX) $$(XR806_CXXFLAGS) -c $$< -o $$@
endef

$(eval $(call XR806_COMPILE_CPP,application,$(XR806_SRC)/esphome/core/application.cpp))
$(eval $(call XR806_COMPILE_CPP,component,$(XR806_SRC)/esphome/core/component.cpp))
$(eval $(call XR806_COMPILE_CPP,scheduler,$(XR806_SRC)/esphome/core/scheduler.cpp))
$(eval $(call XR806_COMPILE_CPP,helpers,$(XR806_SRC)/esphome/core/helpers.cpp))
$(eval $(call XR806_COMPILE_CPP,platform_helpers,$(XR806_SRC)/esphome/components/xr806/helpers.cpp))
$(eval $(call XR806_COMPILE_CPP,hal,$(XR806_SRC)/esphome/components/xr806/hal.cpp))
$(eval $(call XR806_COMPILE_CPP,wake_freertos,$(XR806_SRC)/esphome/core/wake/wake_freertos.cpp))
$(eval $(call XR806_COMPILE_CPP,main,$(XR806_SRC)/main.cpp))
$(eval $(call XR806_COMPILE_CPP,runtime,$(XR806_SRC)/esphome/components/xr806/runtime.cpp))
$(eval $(call XR806_COMPILE_CPP,xr_wifi_standby,$(XR806_SRC)/esphome/components/xr806/wifi_standby.cpp))
$(eval $(call XR806_COMPILE_CPP,xr_api_standby,$(XR806_SRC)/esphome/components/xr806/api_standby.cpp))
$(eval $(call XR806_COMPILE_CPP,xr_mqtt_standby,$(XR806_SRC)/esphome/components/xr806/mqtt_standby.cpp))
$(eval $(call XR806_COMPILE_CPP,xr_http_standby,$(XR806_SRC)/esphome/components/xr806/http_standby.cpp))

$(eval $(call XR806_COMPILE_CPP,xr_socket_compat,$(XR806_SRC)/esphome/components/xr806/socket_compat.cpp))

# Compile unchanged ESPHome socket sources with XR806 prototypes.
XR806_SOCKET_SHIM := $(XR806_SRC)/esphome/components/xr806/socket_compat.h

define XR806_COMPILE_SOCKET_CPP
$(XR806_OBJDIR)/$(1).o: $(2) $(XR806_DEPS) $(XR806_SOCKET_SHIM)
>@mkdir -p $$(@D)
>$$(XR806_CXX) $$(XR806_CXXFLAGS) -include $$(XR806_SOCKET_SHIM) -c $$< -o $$@
endef

$(eval $(call XR806_COMPILE_SOCKET_CPP,xr_socket_core,$(XR806_REPO_ROOT)/esphome/components/socket/socket.cpp))
$(eval $(call XR806_COMPILE_SOCKET_CPP,xr_socket_lwip,$(XR806_REPO_ROOT)/esphome/components/socket/lwip_sockets_impl.cpp))


# Build a separate SDK main.o with ESPHome enabled.
# The vendor project's original main.o remains untouched.

$(XR806_ESPHOME_MAIN): $(PRJ_ROOT_PATH)/main.c
>@mkdir -p $(@D)
>$(CC) $(CC_FLAGS) $(CC_SYMBOLS) \
>    -DTRV16_USE_ESPHOME \
>    -std=gnu99 \
>    $(INCLUDE_PATHS) \
>    -o $@ $<

# Preprocess the WXU 2MiB OTA-enabled AWIH image configuration.
# Layout: boot+app <=1276K; FDCM 1276..1280K; XZ update 1280..1920K; reserved tail 1920..2048K.

XR806_IMAGE_CFG ?= $(XR806_FW_DIR)/image.cfg

$(XR806_IMAGE_CFG): $(ROOT_PATH)/project/demo/trv16_port/image/xr806/image.cfg
>@mkdir -p $(@D)
>$(CC) -E -P -CC $(CC_SYMBOLS) \
>    -I$(ROOT_PATH)/include \
>    -include generated/autoconf.h \
>    -o $@ - < $<

# Build RAM/XIP binaries and package the AWIH firmware image.

XR806_FW_DIR ?= $(error XR806_FW_DIR must be provided by ESPHome)
XR806_AWIH ?= $(XR806_FW_DIR)/firmware.img

$(XR806_FW_DIR)/app.bin: $(XR806_OUTPUT)
>@mkdir -p $(@D)
>$(OBJCOPY) -O binary -R .xip $< $@

$(XR806_FW_DIR)/app_xip.bin: $(XR806_OUTPUT)
>@mkdir -p $(@D)
>$(OBJCOPY) -O binary -j .xip $< $@

$(XR806_FW_DIR)/mkimage: $(ROOT_PATH)/tools/mkimage
>@mkdir -p $(@D)
>install -m 0755 $< $@

$(XR806_FW_DIR)/boot_40M.bin: $(ROOT_PATH)/bin/xradio_v3/boot/xr806/boot_40M.bin
>@mkdir -p $(@D)
>install -m 0644 $< $@

$(XR806_FW_DIR)/wlan_bl.bin: $(ROOT_PATH)/bin/xradio_v3/wlan_bl.bin
>@mkdir -p $(@D)
>install -m 0644 $< $@

$(XR806_FW_DIR)/wlan_fw.bin: $(ROOT_PATH)/bin/xradio_v3/wlan_fw.bin
>@mkdir -p $(@D)
>install -m 0644 $< $@

$(XR806_FW_DIR)/sys_sdd_40M.bin: $(ROOT_PATH)/bin/xradio_v3/sys_sdd_40M.bin
>@mkdir -p $(@D)
>install -m 0644 $< $@

# run_compile() adds the OTA layout to the BOOT header when it copies the image to firmware.bin.
$(XR806_AWIH): $(XR806_FW_DIR)/app.bin $(XR806_FW_DIR)/app_xip.bin $(XR806_IMAGE_CFG) $(XR806_FW_DIR)/mkimage $(XR806_FW_DIR)/boot_40M.bin $(XR806_FW_DIR)/wlan_bl.bin $(XR806_FW_DIR)/wlan_fw.bin $(XR806_FW_DIR)/sys_sdd_40M.bin
>cd $(XR806_FW_DIR) && ./mkimage -c image.cfg -o $(notdir $@)

# snprintf() and friends come from the ROM without '%zu' support; printf_compat.cpp fixes the format first.
XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/printf_compat.o
$(XR806_OUTPUT): $(XR806_OBJDIR)/printf_compat.o
$(XR806_OUTPUT): LD_FLAGS += -Wl,--wrap=snprintf -Wl,--wrap=vsnprintf -Wl,--wrap=sprintf -Wl,--wrap=vsprintf
$(eval $(call XR806_COMPILE_CPP,printf_compat,$(XR806_SRC)/esphome/components/xr806/printf_compat.cpp))

# Additional GPIO object, independent of existing object-list formatting.
XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/gpio.o

# Explicit prerequisite ensures the ELF is relinked when gpio.o changes.
$(XR806_OUTPUT): $(XR806_OBJDIR)/gpio.o

$(eval $(call XR806_COMPILE_CPP,gpio,$(XR806_SRC)/esphome/components/xr806/gpio.cpp))


# ESPHome entity and GPIO binary-sensor implementations.

# Shared core entity implementation.

XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/entity_base.o
$(XR806_OUTPUT): $(XR806_OBJDIR)/entity_base.o

$(eval $(call XR806_COMPILE_CPP,entity_base,$(XR806_SRC)/esphome/core/entity_base.cpp))


# ESPHome core GPIO logging and utilities.
# core_gpio.o avoids collision with XR806 platform gpio.o.

XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/core_gpio.o

$(XR806_OUTPUT): $(XR806_OBJDIR)/core_gpio.o

$(eval $(call XR806_COMPILE_CPP,core_gpio,$(XR806_SRC)/esphome/core/gpio.cpp))


# ESPHome core logging implementation.

XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/core_log.o

$(XR806_OUTPUT): $(XR806_OBJDIR)/core_log.o

$(eval $(call XR806_COMPILE_CPP,core_log,$(XR806_SRC)/esphome/core/log.cpp))

# Platform-neutral core sources that ESPHome copied for this configuration.
# time_64.cpp is not needed (USE_NATIVE_64BIT_TIME) and static_task.cpp is ESP32 only.
XR806_CORE_COMMON := $(foreach name,time alloc_helpers color line_reader util,\
    $(if $(wildcard $(XR806_SRC)/esphome/core/$(name).cpp),$(name)))
XR806_CORE_COMMON_OBJECTS := $(addprefix $(XR806_OBJDIR)/core_,$(addsuffix .o,$(XR806_CORE_COMMON)))
XR806_ESPHOME_OBJECTS += $(XR806_CORE_COMMON_OBJECTS)
$(XR806_OUTPUT): $(XR806_CORE_COMMON_OBJECTS)
$(foreach name,$(XR806_CORE_COMMON),$(eval $(call XR806_COMPILE_CPP,core_$(name),$(XR806_SRC)/esphome/core/$(name).cpp)))

# Discover YAML-generated ESPHome component implementations.
# XR806 platform files are built by their dedicated rules.
# Core files remain explicitly selected to exclude foreign
# platform implementations such as wake_esp8266.cpp.

XR806_AUTO_COMPONENT_SRCS := $(shell find $(XR806_SRC)/esphome/components -type f -name '*.cpp' ! -path '$(XR806_SRC)/esphome/components/xr806/*' 2>/dev/null | LC_ALL=C sort)

# Already compiled with XR806_SOCKET_SHIM.
# Avoid linking the auto-discovered copies alongside xr_socket_*.o.
XR806_AUTO_COMPONENT_SRCS := $(filter-out \
    $(XR806_SRC)/esphome/components/socket/socket.cpp \
    $(XR806_SRC)/esphome/components/socket/lwip_sockets_impl.cpp, \
    $(XR806_AUTO_COMPONENT_SRCS))

XR806_AUTO_COMPONENT_OBJECTS := $(patsubst $(XR806_SRC)/esphome/components/%.cpp,$(XR806_OBJDIR)/auto/components/%.o,$(XR806_AUTO_COMPONENT_SRCS))

XR806_ESPHOME_OBJECTS += $(XR806_AUTO_COMPONENT_OBJECTS)

$(XR806_OUTPUT): $(XR806_AUTO_COMPONENT_OBJECTS)

$(XR806_OBJDIR)/auto/components/%.o: $(XR806_SRC)/esphome/components/%.cpp $(XR806_DEPS)
>@mkdir -p $(@D)
>$(XR806_CXX) $(XR806_CXXFLAGS) -c $< -o $@


# ESPHome OTA's raw DEFLATE decoder is a C file selected by generated
# USE_OTA_DEFLATE. The general auto-component rule only compiles C++.
# Compile this single generated C source as C, with the vendor ARM toolchain.
XR806_OTA_INFLATE_C := $(XR806_SRC)/esphome/components/esphome/ota/ota_esphome_inflate.c
ifneq ($(wildcard $(XR806_OTA_INFLATE_C)),)
XR806_OTA_INFLATE_OBJ := $(XR806_OBJDIR)/xr_ota_inflate.o
XR806_ESPHOME_OBJECTS += $(XR806_OTA_INFLATE_OBJ)
$(XR806_OUTPUT): $(XR806_OTA_INFLATE_OBJ)
$(XR806_OTA_INFLATE_OBJ): $(XR806_OTA_INFLATE_C) $(XR806_DEPS)
>@mkdir -p $(@D)
>$(CC) $(CC_FLAGS) $(CC_SYMBOLS) $(INCLUDE_PATHS) \
>    -I$(XR806_SRC) -include $(XR806_DEFINES) -MMD -MP -c $< -o $@
-include $(XR806_OTA_INFLATE_OBJ:.o=.d)
endif



# Fast select: the lwIP event callback wakes the main loop and the OTA listener
# (see esphome/core/lwip_fast_select.c). It is C and needs the lwIP private headers.
ifneq ($(filter -DUSE_LWIP_FAST_SELECT,$(XR806_EXTRA_CXXFLAGS)),)
XR806_FAST_SELECT_OBJ := $(XR806_OBJDIR)/xr_lwip_fast_select.o
XR806_ESPHOME_OBJECTS += $(XR806_FAST_SELECT_OBJ)
$(XR806_OUTPUT): $(XR806_FAST_SELECT_OBJ)
$(XR806_FAST_SELECT_OBJ): $(XR806_SRC)/esphome/core/lwip_fast_select.c $(XR806_DEPS)
>@mkdir -p $(@D)
>$(CC) $(CC_FLAGS) $(CC_SYMBOLS) $(INCLUDE_PATHS) $(filter -D%,$(XR806_EXTRA_CXXFLAGS)) \
>    -I$(XR806_SRC) -include $(XR806_DEFINES) -MMD -MP -c $< -o $@
-include $(XR806_FAST_SELECT_OBJ:.o=.d)
endif



# Flash-backed preferences manager (FDCM journal in two reserved sectors).

XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/xr806_preferences.o
$(XR806_OUTPUT): $(XR806_OBJDIR)/xr806_preferences.o

$(eval $(call XR806_COMPILE_CPP,xr806_preferences,$(XR806_SRC)/esphome/components/xr806/preferences.cpp))



# Platform-owned network state; no ESPHome Core modifications.
XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/xr_network_status.o
$(XR806_OUTPUT): $(XR806_OBJDIR)/xr_network_status.o

$(eval $(call XR806_COMPILE_CPP,xr_network_status,$(XR806_SRC)/esphome/components/xr806/network_status.cpp))




# ESPHome-generated build metadata + reusable entity iteration (WLAN MAC: helpers.cpp).
XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/xr_core_build_info.o \
                        $(XR806_OBJDIR)/xr_core_component_iterator.o

# The original link target's prerequisites were expanded earlier in this file.
$(XR806_OUTPUT): $(XR806_OBJDIR)/xr_core_build_info.o \
                 $(XR806_OBJDIR)/xr_core_component_iterator.o

$(eval $(call XR806_COMPILE_CPP,xr_core_build_info,$(XR806_SRC)/esphome/core/build_info_data.cpp))
$(eval $(call XR806_COMPILE_CPP,xr_core_component_iterator,$(XR806_SRC)/esphome/core/component_iterator.cpp))

# The lwIP mDNS backend lives in mdns/mdns_xr806.cpp, like the other
# mdns_<platform>.cpp files, and is built by the component loop above.

# Track C++ header dependencies across all ESPHome objects.
# Prevent stale main.o after platform class/header migrations.
XR806_CXXFLAGS += -MMD -MP

# GCC writes one .d file alongside each compiled .o file.
-include $(XR806_ESPHOME_OBJECTS:.o=.d)


# Protocol-neutral production standby coordinator, inert without YAML opt-in.
XR806_ESPHOME_OBJECTS += $(XR806_OBJDIR)/xr_standby_controller.o
$(XR806_OUTPUT): $(XR806_OBJDIR)/xr_standby_controller.o
$(eval $(call XR806_COMPILE_CPP,xr_standby_controller,$(XR806_SRC)/esphome/components/xr806/standby_controller.cpp))
-include $(XR806_OBJDIR)/xr_standby_controller.d

# Restore the default tab recipe prefix for any makefile read after this one.
.RECIPEPREFIX :=
