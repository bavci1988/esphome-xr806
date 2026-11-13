import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.types import ConfigType

from . import CONF_TUYA_TRV16_ID, HUB_SCHEMA, TuyaTRV16, tuya_trv16_ns

DEPENDENCIES = ["tuya_trv16"]

CONF_PROGRAM_DAY = "program_day"
DAY_OPTIONS = [
    "Monday",
    "Tuesday",
    "Wednesday",
    "Thursday",
    "Friday",
    "Saturday",
    "Sunday",
]

TuyaTRV16DaySelect = tuya_trv16_ns.class_(
    "TuyaTRV16DaySelect", select.Select, cg.Parented.template(TuyaTRV16)
)

CONFIG_SCHEMA = HUB_SCHEMA.extend(
    {
        cv.Required(CONF_PROGRAM_DAY): select.select_schema(
            TuyaTRV16DaySelect, entity_category="config", icon="mdi:calendar-week"
        ).extend(
            {
                # The option names shown for Monday .. Sunday
                cv.Optional("options", default=DAY_OPTIONS): cv.All(
                    cv.ensure_list(cv.string_strict), cv.Length(min=7, max=7)
                ),
            }
        ),
    }
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_TUYA_TRV16_ID])
    conf = config[CONF_PROGRAM_DAY]
    day = await select.new_select(conf, options=conf["options"])
    await cg.register_parented(day, hub)
    cg.add(hub.set_day_select(day))
