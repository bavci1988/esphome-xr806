@ The patched WLAN firmware (wlan_fw_patch.py output) in the application's XIP flash. The flash image keeps the stock
@ wlan_fw.bin, which the driver loads at boot; XR806WlanFirmwareSwap restarts the WLAN core with these bytes later.
@ WLAN_FW_PATCHED is the path of the file written at build time.
    .section .xip_rodata.xr806_wlan_fw_keepalive, "a", %progbits
    .balign 4
    .global xr806_wlan_fw_keepalive
xr806_wlan_fw_keepalive:
    .incbin WLAN_FW_PATCHED
    .global xr806_wlan_fw_keepalive_end
xr806_wlan_fw_keepalive_end:
