"""Board definitions for XR806-based modules and bare chips.

WXU source:
https://developer.tuya.com/en/docs/iot/wxu-module-datasheet?id=Kc2xk9qlk04so

GPIO values are (port, pin_number).
Dictionary keys are physical WXU module pad numbers.
"""

# "generic" is a bare XR806A chip on a custom board; "wxu" is the Tuya WXU module.
BOARDS = ("generic", "wxu")

# Every GPIO of the XR806A package (XR806AF2x, 32 pins). The XR806B package (40 pins)
# adds PA0, PA1, PA10 and PA14..PA18, which are not supported.
XR806A_GPIOS = frozenset(
    [("A", pin) for pin in (11, 12, 13, 19, 20, 21, 22, 23)]
    + [("B", pin) for pin in (0, 1, 2, 3, 4, 5, 6, 7, 14, 15)]
)

WXU_GPIO_PADS = {
    1: ("B", 7),
    2: ("B", 4),
    3: ("B", 15),
    4: ("B", 14),
    5: ("A", 11),
    6: ("A", 21),
    7: ("A", 22),
    8: ("B", 3),
    9: ("A", 20),
    10: ("A", 19),
    11: ("A", 13),
    12: ("A", 23),
    15: ("B", 0),
    16: ("B", 1),
    17: ("B", 2),
    19: ("A", 12),
    20: ("B", 5),
    21: ("B", 6),
}

WXU_UART_PADS = {
    0: {"tx": 15, "rx": 16},
    1: {"tx": 4, "rx": 3},
    2: {"tx": 7, "rx": 6},
}

WXU_ADC_PAD = 5

WXU_BOOT_SENSITIVE_PADS = {
    12: "PA23: HIGH during reset enters test mode",
    17: "PB02: LOW during reset enters flashing mode",
}

WXU_NON_GPIO_PADS = {
    13: "GND",
    14: "VBAT",
    18: "RSTN",
}
