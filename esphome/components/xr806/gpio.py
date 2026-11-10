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
    **{("B", pin): "Flash/XIP" for pin in range(2, 8)},
}

_AVAILABLE = frozenset(WXU_GPIO_PADS.values())


def validate_gpio_pin(value: object) -> int:
    """Translate PAx/PBx into the XR806 internal GPIO encoding (PAx = x, PBx = 32 + x)."""
    if not isinstance(value, str):
        raise cv.Invalid("XR806 pins must use PAx or PBx notation, for example PA20")

    value = value.strip().upper()
    if (match := re.fullmatch(r"P([AB])([0-9]{1,2})", value)) is None:
        raise cv.Invalid(
            f"Invalid XR806 GPIO '{value}': expected PA0..PA23 or PB0..PB15"
        )

    bank = match.group(1)
    number = int(match.group(2))
    pin = (bank, number)

    if pin not in _AVAILABLE:
        raise cv.Invalid(f"GPIO {value} is not exposed on the WXU module")
    if (reason := _RESERVED.get(pin)) is not None:
        raise cv.Invalid(f"GPIO {value} is reserved: {reason}")

    return number + (32 if bank == "B" else 0)


def validate_gpio_mode(value: dict[str, bool]) -> dict[str, bool]:
    """Reject modes the XR806 GPIO cannot set up.

    Input together with output is accepted: the pin is an output and reads return its level.
    """
    pins.gpio_validate_modes(value)
    if value[CONF_PULLUP] and value[CONF_PULLDOWN]:
        raise cv.Invalid("Pull-up and pull-down cannot both be enabled")
    return value


XR806_PIN_SCHEMA = pins.gpio_base_schema(
    XR806GPIOPin,
    validate_gpio_pin,
    modes=(CONF_INPUT, CONF_OUTPUT, CONF_PULLUP, CONF_PULLDOWN),
    mode_validator=validate_gpio_mode,
)


@pins.PIN_SCHEMA_REGISTRY.register(DOMAIN, XR806_PIN_SCHEMA)
async def xr806_pin_to_code(config: ConfigType) -> MockObj:
    """Generate XR806GPIOPin instances from ESPHome YAML."""
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_pin(config[CONF_NUMBER]))
    # Only set if true: the C++ default is false.
    if config[CONF_INVERTED]:
        cg.add(var.set_inverted(True))
    cg.add(var.set_flags(pins.gpio_flags_expr(config[CONF_MODE])))
    return var
