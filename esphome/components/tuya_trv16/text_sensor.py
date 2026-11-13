import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.types import ConfigType

from . import CONF_TUYA_TRV16_ID, HUB_SCHEMA

DEPENDENCIES = ["tuya_trv16"]

DAY_KEYS = [
    "monday",
    "tuesday",
    "wednesday",
    "thursday",
    "friday",
    "saturday",
    "sunday",
]

CONFIG_SCHEMA = HUB_SCHEMA.extend(
    {
        cv.Optional(key): text_sensor.text_sensor_schema(
            entity_category="diagnostic", icon="mdi:calendar-clock"
        )
        for key in DAY_KEYS
    }
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_TUYA_TRV16_ID])
    for day, key in enumerate(DAY_KEYS):
        if (conf := config.get(key)) is None:
            continue
        sensor = await text_sensor.new_text_sensor(conf)
        cg.add(hub.set_day_text_sensor(day, sensor))
