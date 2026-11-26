from esphome.components import button
from esphome.components.libretiny import validate_deep_sleep_supported
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG, ICON_POWER
from esphome.types import ConfigType

from . import request_power_off

AUTO_LOAD = ["shutdown"]

CONFIG_SCHEMA = cv.All(
    button.button_schema(
        button.ApplyButton, entity_category=ENTITY_CATEGORY_CONFIG, icon=ICON_POWER
    ),
    validate_deep_sleep_supported,
)


async def to_code(config: ConfigType) -> None:
    request_power_off()
    await button.sub_buttons(config).create(
        config, None, apply="shutdown::shutdown_device()"
    )
