@ Keepalive frame list for the XR806 WLAN firmware patch (wlan_fw.bin); see wlan_fw_patch.py.
@ Assembled at build time with the ARM toolchain and linked at 0x0900A3C8 (WLAN core address).
@ Called with BL from 0x09009E0C instead of "ldr r1,[sp,#0x34]; ldrh r2,[r1,#6]; adds r1,#8",
@ right before the payload memcpy. Returns r1 = source of the payload, r2 = its length; r0 (destination) kept.
@ Server 0 sends frame k = (sequence - TcpSeqInit) / length, the number of frames HA has acknowledged (the
@ firmware adds the length to the sequence for each acknowledged frame). The frames lie in six areas of 128 bytes,
@ 128 / length frames each: the payloads of the three keepalive entries (0x0900BC78, 0x84 apart), then the
@ payloads of the three wake pattern entries (0x0900BE04, 0x84 apart), which the host writes directly and whose
@ Enable stays 0, so the firmware never reads them. Other servers keep one payload.
@ Optionally reports state 2 to the host once W frames have been built (see below).
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
    cmp     r2, #0
    beq     plain
    ldr     r4, =0x0900BC04         @ run-time state of server 0: sequence
    ldr     r4, [r4]
    ldr     r5, =0x0900BB98         @ server table entry 0: TcpSeqInit
    ldr     r5, [r5]
    subs    r4, r4, r5              @ bytes acknowledged
    @ Wake the host when W frames have been built, retries included (W in the unused reserved1 byte of the
    @ keepalive table, counted down here; 0 = off). Built, not acknowledged: a lost acknowledgement must not delay
    @ the wake past the end of the list, where only the host timer is left. The host then wakes through the WLAN
    @ path, never through its own timer while this core sleeps.
    ldr     r5, =0x0900BC73         @ keepalive table: reserved1
    ldrb    r6, [r5]
    cmp     r6, #0
    beq     1f
    subs    r6, r6, #1
    strb    r6, [r5]
    bne     1f
    push    {r0, r1, r2, r3}
    movs    r0, #0                  @ server 0
    movs    r1, #2                  @ state 2: wake the host (as for a matched wake pattern)
    ldr     r3, =0x0900A5B9         @ state report to the host (Thumb)
    blx     r3
    pop     {r0, r1, r2, r3}
1:
    movs    r5, #0                  @ area = 128 // length * length
    movs    r6, #128
1:  cmp     r6, r2
    blo     2f
    subs    r6, r6, r2
    adds    r5, r5, r2
    b       1b
2:  movs    r6, #0                  @ area index a = bytes / area, r4 = bytes % area
3:  cmp     r4, r5
    blo     4f
    subs    r4, r4, r5
    adds    r6, #1
    cmp     r6, #5
    bls     3b
    movs    r6, #5                  @ list exhausted: stay on the last frame (the host wakes before this)
    subs    r4, r5, r2
    b       6f
4:  movs    r3, r4                  @ round r4 down to a frame start
5:  cmp     r3, r2
    blo     7f
    subs    r3, r3, r2
    b       5b
7:  subs    r4, r4, r3
6:  cmp     r6, #3
    blo     8f
    subs    r6, #3
    ldr     r1, =0x0900BE00 - 4     @ wake pattern entry 0, so that "adds r1, #8" points at its payload
8:  movs    r3, #0x84
    muls    r3, r6
    adds    r1, r1, r3
    adds    r1, r1, r4
plain:
    adds    r1, #8
    pop     {r4, r5, r6, pc}
    .ltorg
