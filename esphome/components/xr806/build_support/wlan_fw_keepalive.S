@ Keepalive frame list for the XR806 WLAN firmware patch (wlan_fw.bin); see wlan_fw_patch.py.
@ Assembled at build time with the ARM toolchain. .text is linked at 0x0900A3C8 and .cave2 at 0x0900A4C8 (WLAN core
@ addresses), both in the UDP keepalive, which ESPHome does not use.
@ Frame chooser, called with BL from 0x09009E0C instead of "ldr r1,[sp,#0x34]; ldrh r2,[r1,#6]; adds r1,#8",
@ right before the payload memcpy. Returns r1 = source of the payload, r2 = its length; r0 (destination) kept.
@ Server 0 sends frame k = (sequence - TcpSeqInit) / length, the number of frames HA has acknowledged (the
@ firmware adds the length to the sequence for each acknowledged frame). The frames lie in six areas of 128 bytes,
@ 128 / length frames each: the payloads of the three keepalive entries (0x0900BC78, 0x84 apart), then the
@ payloads of the three wake pattern entries (0x0900BE04, 0x84 apart), which the host writes directly and whose
@ Enable stays 0, so the firmware never reads them. Other servers keep one payload.
@ Optionally stops after W frames, so that the peer's own ping wakes the host (see below).
    .syntax unified
    .thumb
    .text
    .global cave
    .ifndef WAKE_PORT_1
    .equ    WAKE_PORT_1, 80         @ web server
    .endif
    .ifndef WAKE_PORT_2
    .equ    WAKE_PORT_2, 3232       @ OTA
    .endif
    .ifndef WAKE_PORT_3
    .equ    WAKE_PORT_3, 6053       @ native API, so that Home Assistant can connect again
    .endif
    .equ    RUN_STATE, 0x0900BC04   @ run-time state of server 0: +0 sequence, +0xD frame in flight; 0x24 apart
    .equ    KEEPALIVE_TABLE, 0x0900BC70  @ +0 KeepAlivePeriod_s, entry i at +4 + i * 0x84: +2 PayloadLen
    .equ    ACK_HANDLER, 0x0900A720 @ (server, acknowledgement number): counts the frame in flight
cave:
    b       payload                 @ CAVE_ADDRESS: frame chooser
    b       receive_filter          @ CAVE_ADDRESS + 2: drop decision of the receive filter
    b       peer_segment            @ CAVE_ADDRESS + 4: segment from the keepalive peer
payload:
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
    @ Stop after W frames (W in the unused reserved1 byte of the keepalive table, counted down for every frame built,
    @ retries included; 0 = off): with frame W the keepalive period (byte 0x0900BC70) becomes 255 s, so the period
    @ timer the caller restarts next fires after the host's backup timer. Frame W is sent and acknowledged as usual.
    @ Keepalive entry 0 stays enabled: the receive filter only passes Home Assistant's data to the host while it is
    @ (with host sleep set, it drops the data otherwise). Home Assistant then pings after its own idle time, and that
    @ data segment wakes the host through the receive path.
    ldr     r5, =0x0900BC73         @ keepalive table: reserved1
    ldrb    r6, [r5]
    subs    r6, r6, #1
    blo     1f                      @ was 0: off
    strb    r6, [r5]
    bne     1f
    subs    r6, r6, #1              @ 0xFF
    subs    r5, r5, #3
    strb    r6, [r5]                @ 0x0900BC70: KeepAlivePeriod_s
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

    .section .cave2, "ax"
@ Receive filter (0x0900A012), called with BL from 0x0900A216 instead of
@ "ldr r0,[sp,#0x14]; cmp r0,#0; bne 0x0900A220; movs r0,#1", followed by "str r0,[sp,#0x18]".
@ While the host sleeps the stock firmware drops every IPv4 packet that matched no wake filter ([sp,#0x14] == 0).
@ Someone opening the web server, an OTA or an API connection now wakes the host: a TCP segment that is not a
@ fragment, has only SYN of the six flags set and goes to WAKE_PORT_1, 2 or 3 is passed on. A RST or FIN on a
@ keepalive connection is passed on too, so that the host learns at once that Home Assistant closed it; the firmware
@ dropped it, and the host only found out when the keepalive gave up. Everything else is still dropped.
@ r4 = matching server (3: none), r6 = IPv4 header, r5 = r6 + header length.
@ Returns r0 = the new [sp,#0x18]: 1 drops, 0 passes, the old value keeps.
receive_filter:
    ldr     r0, [sp, #0x14]
    cmp     r0, #0
    bne     keep
    ldrb    r0, [r6, #9]            @ protocol
    cmp     r0, #6
    bne     drop
    ldrb    r0, [r6, #0]            @ header length in words
    lsls    r0, r0, #28
    lsrs    r0, r0, #28
    cmp     r0, #5
    blo     drop
    ldrb    r0, [r6, #6]            @ more fragments, fragment offset (high bits)
    lsls    r0, r0, #26
    bne     drop
    ldrb    r0, [r6, #7]            @ fragment offset (low bits)
    cmp     r0, #0
    bne     drop
    ldrb    r0, [r5, #13]           @ TCP flags: URG ACK PSH RST SYN FIN
    lsls    r0, r0, #26
    lsrs    r0, r0, #26
    cmp     r4, #3
    bhs     1f
    movs    r1, #5                  @ RST or FIN
    tst     r0, r1
    bne     pass
1:  cmp     r0, #2                  @ SYN alone
    bne     drop
    ldrb    r0, [r5, #2]            @ destination port
    lsls    r0, r0, #8
    ldrb    r1, [r5, #3]
    orrs    r0, r1
    ldr     r1, =WAKE_PORT_1
    cmp     r0, r1
    beq     keep
    ldr     r1, =WAKE_PORT_2
    cmp     r0, r1
    beq     keep
    ldr     r1, =WAKE_PORT_3
    cmp     r0, r1
    bne     drop
keep:
    ldr     r0, [sp, #0x18]
    bx      lr
pass:
    movs    r0, #0
    bx      lr
drop:
    movs    r0, #1
    bx      lr

@ Segment with ACK set from the peer of keepalive server r4, called with BL from 0x0900A140 instead of the stock
@ "ACK received" handling. r0 = its sequence number, r5 = TCP header, caller's [sp,#8] = its data length.
@ The stock firmware took any such segment for the acknowledgement of the frame in flight. A duplicate ACK, or a
@ ping from Home Assistant that crossed the frame, then counted a frame that was lost on the air: the firmware moved
@ on to the next frame, never sent the lost one again, and Home Assistant waited for its bytes until it gave up on
@ the connection. The frame now only counts when the acknowledgement number is its sequence plus its length.
@ The firmware's own acknowledgement number stays at the peer's sequence number: data from the peer is left for
@ the host, so the peer sends it again if this copy is dropped.
@ Returns r0 = 1 to pass the segment to the host, 0 when the firmware used it up (it is dropped):
@ - not TCP (same ports): used up;
@ - RST or FIN: to the host;
@ - no data: used up;
@ - data that also acknowledged the frame: used up, as passing such a segment to the sleeping host stalled the WLAN
@   core; the peer sends the data again and that copy goes to the host;
@ - other data: to the host, which it wakes.
peer_segment:
    push    {r4, r5, r6, r7, lr}
    ldrb    r1, [r6, #9]            @ protocol: the firmware matches servers by port only, UDP included
    cmp     r1, #6
    bne     4f
    movs    r6, r0
    movs    r3, #0x24
    muls    r3, r4
    ldr     r7, =RUN_STATE
    adds    r7, r7, r3
    movs    r2, #0                  @ r2 = 1: this segment acknowledged the frame in flight
    ldrb    r3, [r7, #0xD]
    cmp     r3, #1
    bne     2f
    movs    r3, #0x84
    muls    r3, r4
    ldr     r2, =KEEPALIVE_TABLE
    adds    r2, r2, r3
    ldrh    r2, [r2, #6]            @ PayloadLen of entry i
    ldr     r3, [r7]
    adds    r2, r2, r3              @ the acknowledgement number that counts the frame
    ldrb    r3, [r5, #8]            @ acknowledgement number, big endian
    ldrb    r1, [r5, #9]
    lsls    r3, r3, #8
    orrs    r3, r1
    ldrb    r1, [r5, #10]
    lsls    r3, r3, #8
    orrs    r3, r1
    ldrb    r1, [r5, #11]
    lsls    r3, r3, #8
    orrs    r3, r1
    cmp     r3, r2
    bne     5f
    movs    r0, r4
    movs    r1, r6
    ldr     r3, =ACK_HANDLER + 1
    bl      call_r3
    movs    r2, #1
    b       2f
5:  movs    r2, #0
2:  ldrb    r0, [r5, #13]           @ TCP flags
    movs    r1, #5                  @ RST or FIN
    tst     r0, r1
    bne     3f
    ldr     r0, [sp, #(20 + 8)]     @ caller's [sp,#8]: data length
    cmp     r0, #0
    beq     4f
    cmp     r2, #0
    bne     4f
3:  movs    r0, #1
    pop     {r4, r5, r6, r7, pc}
4:  movs    r0, #0
    pop     {r4, r5, r6, r7, pc}
call_r3:
    bx      r3
    .ltorg
