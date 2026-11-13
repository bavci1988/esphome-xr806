import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import UNIT_CELSIUS
from esphome.types import ConfigType

from . import CONF_TUYA_TRV16_ID, HUB_SCHEMA, tuya_trv16_ns

DEPENDENCIES = ["tuya_trv16"]

TuyaTRV16PointTemperature = tuya_trv16_ns.class_(
    "TuyaTRV16PointTemperature", number.Number
)

POINT_KEYS = [f"point_{i}_temperature" for i in range(1, 7)]

CONFIG_SCHEMA = HUB_SCHEMA.extend(
    {
        cv.Optional(key): number.number_schema(
            TuyaTRV16PointTemperature,
            unit_of_measurement=UNIT_CELSIUS,
            entity_category="config",
            icon="mdi:thermometer",
        )
        for key in POINT_KEYS
    }
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_TUYA_TRV16_ID])
    for index, key in enumerate(POINT_KEYS):
        if (conf := config.get(key)) is None:
            continue
        temperature = await number.new_number(conf, min_value=5, max_value=35, step=0.5)
        cg.add(hub.set_point_temperature(index, temperature))
