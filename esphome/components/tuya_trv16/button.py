import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.types import ConfigType

from . import CONF_TUYA_TRV16_ID, HUB_SCHEMA, TuyaTRV16, tuya_trv16_ns

DEPENDENCIES = ["tuya_trv16"]

TuyaTRV16SaveButton = tuya_trv16_ns.class_(
    "TuyaTRV16SaveButton", button.Button, cg.Parented.template(TuyaTRV16)
)

# Days each button writes: the chosen day is resolved at run time (0), Monday-Friday, all days.
SAVE_BUTTONS = {
    "save_day": 0,
    "save_workdays": 0x1F,
    "save_all_days": 0x7F,
}

TuyaTRV16PowerOnButton = tuya_trv16_ns.class_(
    "TuyaTRV16PowerOnButton", button.Button, cg.Parented.template(TuyaTRV16)
)

CONF_POWER_ON = "power_on"

CONFIG_SCHEMA = HUB_SCHEMA.extend(
    {
        **{
            cv.Optional(key): button.button_schema(
                TuyaTRV16SaveButton, entity_category="config", icon="mdi:content-save"
            )
            for key in SAVE_BUTTONS
        },
        # Switches the thermostat on, e.g. while it waits for its button after a battery change
        cv.Optional(CONF_POWER_ON): button.button_schema(
            TuyaTRV16PowerOnButton, icon="mdi:power"
        ),
    }
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_TUYA_TRV16_ID])
    for key, day_mask in SAVE_BUTTONS.items():
        if (conf := config.get(key)) is None:
            continue
        save = await button.new_button(conf, day_mask)
        await cg.register_parented(save, hub)
    if (conf := config.get(CONF_POWER_ON)) is not None:
        power_on = await button.new_button(conf)
        await cg.register_parented(power_on, hub)
