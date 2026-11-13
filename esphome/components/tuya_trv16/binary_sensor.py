import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_PROBLEM
from esphome.types import ConfigType

from . import CONF_TUYA_TRV16_ID, HUB_SCHEMA

DEPENDENCIES = ["tuya_trv16"]

CONF_AWAITING_CONFIRMATION = "awaiting_confirmation"

CONFIG_SCHEMA = HUB_SCHEMA.extend(
    {
        # After a battery change the thermostat stays off until its button is pressed.
        cv.Optional(CONF_AWAITING_CONFIRMATION): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_PROBLEM, icon="mdi:gesture-tap-button"
        ),
    }
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_TUYA_TRV16_ID])
    if (conf := config.get(CONF_AWAITING_CONFIRMATION)) is not None:
        sensor = await binary_sensor.new_binary_sensor(conf)
        cg.add(hub.set_awaiting_confirmation_binary_sensor(sensor))
