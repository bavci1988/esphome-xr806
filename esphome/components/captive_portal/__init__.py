import logging

import esphome.codegen as cg
from esphome.components import web_server_base, wifi
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.components.wifi import DOMAIN as WIFI_DOMAIN
import esphome.config_validation as cv
from esphome.const import (
    CONF_AP,
    CONF_COMPRESSION,
    CONF_ID,
    PLATFORM_BK72XX,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    PLATFORM_LN882X,
    PLATFORM_RP2,
    PLATFORM_RTL87XX,
    PLATFORM_XR806,
)
from esphome.core import CORE, coroutine_with_priority
from esphome.coroutine import CoroPriority
import esphome.final_validate as fv
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)


def AUTO_LOAD() -> list[str]:
    if CORE.is_xr806:
        # The native XR806 captive backend does not depend on Arduino/IDF
        # WebServerBase or the unsupported HTTP OTA upload handler.
        return []
    auto_load = ["web_server_base", "ota.web_server"]
    if CORE.is_esp32:
        auto_load.append("socket")
    return auto_load


DEPENDENCIES = ["wifi"]
CODEOWNERS = ["@esphome/core"]
DOMAIN = "captive_portal"

captive_portal_ns = cg.esphome_ns.namespace("captive_portal")
CaptivePortal = captive_portal_ns.class_("CaptivePortal", cg.Component)

def _validate_schema(config: ConfigType) -> ConfigType:
    """Select only the backend-specific ID dependency during validation.

    XR806 has its own bounded native lwIP server and must not auto-load
    web_server_base, which currently requires Arduino or ESP-IDF.
    """
    fields = {
        cv.GenerateID(): cv.declare_id(CaptivePortal),
        cv.Optional(CONF_COMPRESSION, default="gzip"): cv.one_of("gzip", "br"),
    }
    if not CORE.is_xr806:
        fields[cv.GenerateID(CONF_WEB_SERVER_BASE_ID)] = cv.use_id(
            web_server_base.WebServerBase
        )
    return cv.Schema(fields).extend(cv.COMPONENT_SCHEMA)(config)


CONFIG_SCHEMA = cv.All(
    _validate_schema,
    cv.only_on(
        [
            PLATFORM_ESP32,
            PLATFORM_ESP8266,
            PLATFORM_BK72XX,
            PLATFORM_LN882X,
            PLATFORM_RP2,
            PLATFORM_RTL87XX,
            PLATFORM_XR806,
        ]
    ),
)


def _final_validate(config: ConfigType) -> None:
    full_config = fv.full_config.get()
    wifi_conf = full_config.get(WIFI_DOMAIN)

    if wifi_conf is None:
        # This shouldn't happen due to DEPENDENCIES = ["wifi"], but check anyway
        raise cv.Invalid("Captive portal requires the wifi component to be configured")

    if CONF_AP not in wifi_conf:
        _LOGGER.warning(
            "Captive portal is enabled but no WiFi AP is configured. "
            "The captive portal will not be accessible. "
            "Add 'ap:' to your WiFi configuration to enable the captive portal."
        )

    if not CORE.is_xr806:
        web_server_base.consume_captive_dns_sockets(config, DOMAIN)


FINAL_VALIDATE_SCHEMA = _final_validate


@coroutine_with_priority(CoroPriority.CAPTIVE_PORTAL)
async def to_code(config: ConfigType) -> None:
    if CORE.is_xr806:
        # Keep the public "captive_portal:" YAML and component interface.
        # The XR806 implementation uses its own lwIP HTTP/DNS transport.
        cg.add_define("USE_CAPTIVE_PORTAL")
        cg.add_define("USE_XR806_HTTP_PORTAL")
        cg.add_global(
            cg.RawStatement(
                '#include "esphome/components/captive_portal/captive_portal.h"'
            )
        )
        var = cg.new_Pvariable(config[CONF_ID])
        await cg.register_component(var, config)
        return

    paren = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])

    var = cg.new_Pvariable(config[CONF_ID], paren)
    await cg.register_component(var, config)
    cg.add_define("USE_CAPTIVE_PORTAL")
    # The portal reads wifi scan results from the web server task; this makes the
    # wifi component guard them with a lock on multi-threaded platforms.
    wifi.request_wifi_scan_results_lock()

    if config[CONF_COMPRESSION] == "gzip":
        cg.add_define("USE_CAPTIVE_PORTAL_GZIP")

    web_server_base.add_captive_dns_library()
