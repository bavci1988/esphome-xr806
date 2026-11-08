"""Board definitions for XR806-based modules.

WXU source:
https://developer.tuya.com/en/docs/iot/wxu-module-datasheet?id=Kc2xk9qlk04so

GPIO values are (port, pin_number).
Dictionary keys are physical WXU module pad numbers.
"""

WXU_GPIO_PADS = {
    1:  ("B", 7),
    2:  ("B", 4),
    3:  ("B", 15),
    4:  ("B", 14),
    5:  ("A", 11),
    6:  ("A", 21),
    7:  ("A", 22),
    8:  ("B", 3),
    9:  ("A", 20),
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
