"""Standby of a BK7236 (LibreTiny) module that a Tuya MCU running protocol low_power controls.

The module keeps its Wi-Fi link and lets ARMINO's low-voltage sleep run only while the MCU has
agreed to sleep. The MCU wakes the module through its wake line before it sends.
"""

import esphome.codegen as cg
from esphome.components import tuya
from esphome.components.libretiny import get_libretiny_family
from esphome.components.libretiny.const import FAMILY_BK7236
import esphome.config_validation as cv
from esphome.const import (
    CONF_BOARD,
    CONF_ID,
    CONF_OPTIONS,
    CONF_POWER_SAVE_MODE,
    CONF_PROTOCOL,
    CONF_UART_ID,
)
from esphome.core import CORE
import esphome.final_validate as fv
from esphome.types import ConfigType

CODEOWNERS = ["@bavci1988"]
DEPENDENCIES = ["bk72xx", "tuya", "wifi"]
DOMAIN = "bk7236_tuya_standby"

CONF_BOOT_HOLD = "boot_hold"
# The MCU wake line (module output) on boards whose Tuya pinout is known, used until the MCU names its own
DEFAULT_MCU_WAKE = {"t3-u": 16}
CONF_TUYA_ID = "tuya_id"

bk7236_tuya_standby_ns = cg.esphome_ns.namespace("bk7236_tuya_standby")
BK7236TuyaStandby = bk7236_tuya_standby_ns.class_("BK7236TuyaStandby", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BK7236TuyaStandby),
        cv.GenerateID(CONF_TUYA_ID): cv.use_id(tuya.TuyaUART),
        # No standby right after a start, so that a faulty image can still be replaced over the air
        cv.Optional(
            CONF_BOOT_HOLD, default="60s"
        ): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config: ConfigType) -> ConfigType:
    if get_libretiny_family() != FAMILY_BK7236:
        raise cv.Invalid(f"{DOMAIN} only supports the BK7236 family")
    full = fv.full_config.get()
    if (full.get("tuya") or {}).get(CONF_PROTOCOL) != tuya.PROTOCOL_LOW_POWER:
        raise cv.Invalid(f"{DOMAIN} needs tuya with protocol: low_power")
    options = full["bk72xx"]["framework"].get(CONF_OPTIONS) or {}
    if str(options.get("LT_AUTO_LIGHT_SLEEP", 0)) != "1":
        raise cv.Invalid(
            f"{DOMAIN} needs the CPU sleep of LibreTiny: set "
            "bk72xx: framework: options: LT_AUTO_LIGHT_SLEEP: 1"
        )
    if str(full["wifi"].get(CONF_POWER_SAVE_MODE, "NONE")).upper() == "NONE":
        raise cv.Invalid(
            f"{DOMAIN} keeps the Wi-Fi link during standby: set wifi: power_save_mode: light"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_TUYA_ID])
    # The MCU's UART: its receiver is switched off while the MCU sleeps
    bus = await cg.get_variable(CORE.config["tuya"][CONF_UART_ID])
    var = cg.new_Pvariable(config[CONF_ID], hub, bus, config[CONF_BOOT_HOLD])
    await cg.register_component(var, config)
    if (pin := DEFAULT_MCU_WAKE.get(CORE.config["bk72xx"][CONF_BOARD])) is not None:
        cg.add(var.set_default_mcu_wake(pin))
