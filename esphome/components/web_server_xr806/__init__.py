"""XR806 native lwIP HTTP transport for ESPHome web_server and captive_portal."""

from esphome.config_helpers import filter_source_files_from_defines
import esphome.config_validation as cv

CODEOWNERS = ["@bavci1988"]
DOMAIN = "web_server_xr806"
DEPENDENCIES = ["network", "socket"]

CONFIG_SCHEMA = cv.Schema({})

# The native transport is only linked when an HTTP consumer pulls this module in.
FILTER_SOURCE_FILES = filter_source_files_from_defines(
    {"web_server_xr806.cpp": "USE_XR806"}
)
