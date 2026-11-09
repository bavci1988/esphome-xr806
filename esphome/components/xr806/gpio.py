"""Native GPIO support for the Allwinner/XRadio XR806."""

import re

from esphome import pins
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INPUT,
    CONF_INVERTED,
    CONF_MODE,
    CONF_NUMBER,
    CONF_OUTPUT,
    CONF_PULLDOWN,
    CONF_PULLUP,
)
from esphome.cpp_generator import MockObj
from esphome.types import ConfigType

from . import DOMAIN
from .boards import WXU_GPIO_PADS

xr806_ns = cg.esphome_ns.namespace("xr806")

XR806GPIOPin = xr806_ns.class_(
    "XR806GPIOPin",
    cg.InternalGPIOPin,
)

# Reserved until board/peripheral ownership is configurable. UART0 (PB0/PB1) is
# usable: the uart: component claims it, and ESPHome rejects a pin used twice.
_RESERVED = {
    ("A", 21): "UART2 RX",
    ("A", 22): "UART2 TX",
    ("A", 23): "Boot/test mode",
    ("B", 14): "UART1 TX",
    ("B", 15): "UART1 RX",
}

for _pin in range(2, 8):
    _RESERVED[("B", _pin)] = "Flash/XIP"

_AVAILABLE = set(WXU_GPIO_PADS.values())


def validate_gpio_pin(value: object) -> int:
    """Translate PAx/PBx into the XR806 internal GPIO encoding."""
    if not isinstance(value, str):
        raise cv.Invalid("XR806 pins must use PAx or PBx notation, for example PA20")

    value = value.strip().upper()

    match = re.fullmatch(r"P([AB])(0|[1-9][0-9]?)", value)

    if match is None:
        raise cv.Invalid(
            f"Invalid XR806 GPIO '{value}': expected PA0..PA23 or PB0..PB15"
        )

    bank = match.group(1)
    number = int(match.group(2))
    pin = (bank, number)

    if pin not in _AVAILABLE:
        raise cv.Invalid(f"GPIO {value} is not exposed on the WXU module")

    if pin in _RESERVED:
        raise cv.Invalid(f"GPIO {value} is reserved: {_RESERVED[pin]}")

    # Internal encoding: PAx=x, PBx=32+x.
    return number + (32 if bank == "B" else 0)


def validate_gpio_usage(config: ConfigType) -> ConfigType:
    """Accept only GPIO modes implemented by the XR806 HAL."""
    mode = config[CONF_MODE]

    is_input = mode[CONF_INPUT]
    is_output = mode[CONF_OUTPUT]
    pullup = mode[CONF_PULLUP]
    pulldown = mode[CONF_PULLDOWN]

    if is_input == is_output:
        raise cv.Invalid("XR806 GPIO must be either INPUT or OUTPUT")

    if pullup and pulldown:
        raise cv.Invalid("Pull-up and pull-down cannot both be enabled")

    if is_output and (pullup or pulldown):
        raise cv.Invalid("XR806 output GPIO does not support pull resistors")

    return config


XR806_PIN_SCHEMA = pins.gpio_base_schema(
    XR806GPIOPin,
    validate_gpio_pin,
    modes=[
        CONF_INPUT,
        CONF_OUTPUT,
        CONF_PULLUP,
        CONF_PULLDOWN,
    ],
).add_extra(validate_gpio_usage)


@pins.PIN_SCHEMA_REGISTRY.register(DOMAIN, XR806_PIN_SCHEMA)
async def xr806_pin_to_code(config: ConfigType) -> MockObj:
    """Generate XR806GPIOPin instances from ESPHome YAML."""
    var = cg.new_Pvariable(config[CONF_ID])

    cg.add(var.set_pin(config[CONF_NUMBER]))

    if config[CONF_INVERTED]:
        cg.add(var.set_inverted(True))

    cg.add(var.set_flags(pins.gpio_flags_expr(config[CONF_MODE])))

    return var
