"""XR806 pins that ESPHome must not drive."""

import esphome.config_validation as cv

# GPIO numbers: PAx is x, PBx is 32 + x. As in the native XR806 port: the code runs from the
# flash on PB2/PB3/PB5/PB6/PB7 (PB4 is free; the original WXU firmware drives it as the MCU
# wake output), and PA23 selects the boot/test mode.
_RESERVED = {
    23: "boot/test mode",
    **{32 + pin: "flash (code runs from it)" for pin in (2, 3, 5, 6, 7)},
}


def validate_pin(value: int) -> int:
    if (reason := _RESERVED.get(value)) is not None:
        name = f"PA{value}" if value < 32 else f"PB{value - 32}"
        raise cv.Invalid(f"Pin {name} is reserved: {reason}")
    return value
