"""XR806 lwIP HTTP transport for the shared web_server, captive_portal and OTA handlers."""

import esphome.codegen as cg
from esphome.components import web_server_base
from esphome.components.web_server import AUTH_TYPE_DIGEST
from esphome.config_helpers import filter_source_files_from_defines
import esphome.config_validation as cv
from esphome.const import CONF_AUTH, CONF_ID, CONF_TYPE
import esphome.final_validate as fv
from esphome.types import ConfigType

CODEOWNERS = ["@bavci1988"]
DOMAIN = "web_server_xr806"
DEPENDENCIES = ["network"]
AUTO_LOAD = ["socket"]

web_server_xr806_ns = cg.esphome_ns.namespace("web_server_xr806")
WebServerXR806 = web_server_xr806_ns.class_("WebServerXR806", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WebServerXR806),
        cv.GenerateID(web_server_base.CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
    }
)


def _validate_auth(config: ConfigType) -> ConfigType:
    """The XR806 server checks Basic credentials only."""
    web_server = fv.full_config.get().get("web_server")
    if (
        web_server is not None
        and (auth := web_server.get(CONF_AUTH)) is not None
        and auth.get(CONF_TYPE) == AUTH_TYPE_DIGEST
    ):
        raise cv.Invalid(
            "web_server on XR806 supports Basic authentication only; "
            "set 'auth: type: basic'"
        )
    return config


FINAL_VALIDATE_SCHEMA = _validate_auth


async def to_code(config: ConfigType) -> None:
    base = await cg.get_variable(config[web_server_base.CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)


FILTER_SOURCE_FILES = filter_source_files_from_defines(
    {
        "web_server_xr806.cpp": "USE_XR806",
        "web_server_xr806_component.cpp": "USE_XR806",
    }
)
