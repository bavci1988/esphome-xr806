@ Keepalive frame list for the XR806 WLAN firmware patch (wlan_fw.bin); see wlan_fw_patch.py.
@ Assembled at build time with the ARM toolchain and linked at 0x0900A3C8 (WLAN core address).
@ Called with BL from 0x09009E0C instead of "ldr r1,[sp,#0x34]; ldrh r2,[r1,#6]; adds r1,#8",
@ right before the payload memcpy. Returns r1 = source of the payload, r2 = its length; r0 (destination) kept.
@ Server 0 sends frame k = IP ID - IPIdInit, the number of frames HA has acknowledged, from the payload areas of
@ the three keepalive entries (n = 128 / length frames each, entries 0x84 apart). Other servers keep one payload.
    .syntax unified
    .thumb
    .text
    .global cave
cave:
    push    {r4, r5, r6, lr}
    ldr     r1, [sp, #0x44]         @ caller's [sp,#0x34]: keepalive entry i
    ldrh    r2, [r1, #6]            @ frame length
    ldr     r3, [sp, #0x4c]         @ caller's [sp,#0x3c]: server index i
    cmp     r3, #0
    bne     plain
    ldr     r4, =0x0900BC0C         @ run-time state of server 0: IP ID
    ldr     r4, [r4]
    ldr     r5, =0x0900BB96         @ server table entry 0: IPIdInit
    ldrh    r5, [r5]
    subs    r4, r4, r5
    lsls    r4, r4, #16
    lsrs    r4, r4, #16             @ k
    movs    r5, #0                  @ n = 128 / length
    movs    r6, #128
    cmp     r2, #0
    beq     plain
1:  cmp     r6, r2
    blo     2f
    subs    r6, r6, r2
    adds    r5, #1
    b       1b
2:  cmp     r5, #0
    beq     plain
    movs    r6, #0                  @ entry e = k / n, r4 = k % n
3:  cmp     r4, r5
    blo     4f
    subs    r4, r4, r5
    adds    r6, #1
    b       3b
4:  cmp     r6, #2
    bls     5f
    movs    r6, #2                  @ list exhausted: stay on the last frame (the host wakes before this)
    subs    r4, r5, #1
5:  movs    r3, #0x84
    muls    r3, r6
    adds    r1, r1, r3
    muls    r4, r2
    adds    r1, r1, r4
plain:
    adds    r1, #8
    pop     {r4, r5, r6, pc}
    .ltorg
