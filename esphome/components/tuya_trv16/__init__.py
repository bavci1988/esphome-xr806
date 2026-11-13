import esphome.codegen as cg
from esphome.components import tuya
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.types import ConfigType

CODEOWNERS = ["@bavci1988"]
DEPENDENCIES = ["tuya"]
DOMAIN = "tuya_trv16"

CONF_TUYA_TRV16_ID = "tuya_trv16_id"
CONF_FORCE_MANUAL_MODE = "force_manual_mode"

tuya_trv16_ns = cg.esphome_ns.namespace("tuya_trv16")
TuyaTRV16 = tuya_trv16_ns.class_("TuyaTRV16", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TuyaTRV16),
        cv.GenerateID(tuya.CONF_TUYA_ID): cv.use_id(tuya.Tuya),
        # Switch to manual mode after each start, so the built-in weekly program does not change the target
        cv.Optional(CONF_FORCE_MANUAL_MODE, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)

HUB_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TUYA_TRV16_ID): cv.use_id(TuyaTRV16),
    }
)


async def to_code(config: ConfigType) -> None:
    tuya_hub = await cg.get_variable(config[tuya.CONF_TUYA_ID])
    var = cg.new_Pvariable(config[CONF_ID], tuya_hub)
    await cg.register_component(var, config)
    if config[CONF_FORCE_MANUAL_MODE]:
        cg.add(var.set_force_manual_mode(True))
