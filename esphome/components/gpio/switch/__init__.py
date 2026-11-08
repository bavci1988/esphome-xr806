from esphome import pins
import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import CONF_INTERLOCK, CONF_PIN
from esphome.types import ConfigType

from .. import gpio_ns
from esphome.core import CORE
from esphome.const import CONF_RESTORE_MODE, CONF_RESTORE_STATE, CONF_STATE

GPIOSwitch = gpio_ns.class_("GPIOSwitch", switch.Switch, cg.Component)

CONF_INTERLOCK_WAIT_TIME = "interlock_wait_time"
CONFIG_SCHEMA = (
    switch.switch_schema(GPIOSwitch)
    .extend(
        {
            cv.Required(CONF_PIN): pins.gpio_output_pin_schema,
            cv.Optional(
                CONF_INTERLOCK, visibility=cv.Visibility.ADVANCED
            ): cv.ensure_list(cv.use_id(switch.Switch)),
            cv.Optional(
                CONF_INTERLOCK_WAIT_TIME,
                default="0ms",
                visibility=cv.Visibility.ADVANCED,
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config: ConfigType) -> None:
    var = await switch.new_switch(config)
    await cg.register_component(var, config)

    await cg.add_pin(var.set_pin, config[CONF_PIN])

    if (interlock := config.get(CONF_INTERLOCK)) is not None:
        cg.add_define("USE_GPIO_SWITCH_INTERLOCK")
        locks = [await cg.get_variable(it) for it in interlock]
        # The group's table lists this switch too, so every member shares one table.
        group = sorted({str(s): s for s in (var, *locks)}.values(), key=str)
        if len(group) > 1:
            table = cg.shared_progmem_array(
                "gpio_interlock",
                switch.Switch.operator("ptr"),
                cg.ArrayInitializer(*group),
                constexpr=False,
            )
            cg.add(var.set_interlock(table, len(group)))
        cg.add(var.set_interlock_wait_time(config[CONF_INTERLOCK_WAIT_TIME]))


# XR806_SWITCH_RESTORE_VALIDATION_V2
# XR806 preferences currently have no persistent storage.
def _validate_xr806_restore(config):
    if CORE.target_platform != "xr806":
        return config

    mode = config.get(CONF_RESTORE_MODE)

    # ESPHome supplies an EStr containing the YAML name here,
    # not the C++ MockObj from switch.RESTORE_MODES.
    if mode is not None and str(mode) not in (
        "ALWAYS_OFF",
        "ALWAYS_ON",
        "DISABLED",
    ):
        raise cv.Invalid(
            "XR806 GPIO switch has no persistent preference "
            "storage yet. Use ALWAYS_OFF, ALWAYS_ON or DISABLED."
        )

    restore_state = config.get(CONF_RESTORE_STATE)

    if restore_state is not None:
        state = str(restore_state.get(CONF_STATE, "INITIAL"))

        if state != "INITIAL":
            raise cv.Invalid(
                "XR806 GPIO switch cannot restore saved states yet. "
                "Use restore_state with state: INITIAL or "
                "restore_mode: ALWAYS_OFF/ALWAYS_ON."
            )

    return config


FINAL_VALIDATE_SCHEMA = _validate_xr806_restore
