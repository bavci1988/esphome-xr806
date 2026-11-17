# Async TCP client support for all platforms
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.core import CORE, CoroPriority, coroutine_with_priority
from esphome.types import ConfigType

CODEOWNERS = ["@esphome/core"]
DEPENDENCIES = ["network"]


def AUTO_LOAD() -> list[str]:
    # Socket component needed for platforms using socket-based implementation
    # ESP32, ESP8266, RP2040, and LibreTiny use AsyncTCP libraries, others use sockets
    if (
        not CORE.is_esp32
        and not CORE.is_esp8266
        and not CORE.is_rp2
        and not CORE.is_libretiny
    ):
        return ["socket"]
    return []


# Support all platforms - Arduino/ESP-IDF get libraries, other platforms use socket implementation
CONFIG_SCHEMA = cv.Schema({})


@coroutine_with_priority(CoroPriority.NETWORK_TRANSPORT)
async def to_code(config: ConfigType) -> None:
    if CORE.is_esp32:
        # https://github.com/ESP32Async/AsyncTCP
        from esphome.components.esp32 import add_idf_component

        add_idf_component(name="esp32async/asynctcp", ref="3.4.91")
    elif CORE.is_libretiny:
        # https://github.com/ESP32Async/AsyncTCP
        cg.add_library("ESP32Async/AsyncTCP", "3.4.5")
        from esphome.components.libretiny import get_libretiny_family
        from esphome.components.libretiny.const import FAMILY_BK7236

        if get_libretiny_family() == FAMILY_BK7236:
            # AsyncTCP's default task priority (10) is out of range on the
            # ARMINO SDK (configMAX_PRIORITIES is 10). Stay below the main
            # loop (6) and the TCP/IP thread (7), like on ESP32.
            cg.add_build_flag("-DCONFIG_ASYNC_TCP_PRIORITY=5")
    elif CORE.is_esp8266:
        # https://github.com/ESP32Async/ESPAsyncTCP
        cg.add_library("ESP32Async/ESPAsyncTCP", "2.0.0")
    elif CORE.is_rp2:
        # https://github.com/ayushsharma82/RPAsyncTCP
        # RPAsyncTCP is a drop-in replacement for AsyncTCP_RP2040W with better
        # ESPAsyncWebServer compatibility
        cg.add_library("ayushsharma82/RPAsyncTCP", "1.3.2")
    # Other platforms (host, etc) use socket-based implementation


def FILTER_SOURCE_FILES() -> list[str]:
    # Exclude socket implementation for platforms that use AsyncTCP libraries
    if CORE.is_esp32 or CORE.is_esp8266 or CORE.is_rp2 or CORE.is_libretiny:
        return ["async_tcp_socket.cpp"]
    return []
