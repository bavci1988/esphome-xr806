import esphome.codegen as cg
from esphome.components import datetime
import esphome.config_validation as cv
from esphome.types import ConfigType

from . import CONF_TUYA_TRV16_ID, HUB_SCHEMA, tuya_trv16_ns

DEPENDENCIES = ["tuya_trv16"]

TuyaTRV16PointTime = tuya_trv16_ns.class_("TuyaTRV16PointTime", datetime.TimeEntity)

POINT_KEYS = [f"point_{i}_time" for i in range(1, 7)]

CONFIG_SCHEMA = HUB_SCHEMA.extend(
    {
        cv.Optional(key): datetime.time_schema(TuyaTRV16PointTime).extend(
            {cv.Optional("entity_category", default="config"): cv.entity_category}
        )
        for key in POINT_KEYS
    }
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_TUYA_TRV16_ID])
    for index, key in enumerate(POINT_KEYS):
        if (conf := config.get(key)) is None:
            continue
        time = await datetime.new_datetime(conf)
        cg.add(hub.set_point_time(index, time))
