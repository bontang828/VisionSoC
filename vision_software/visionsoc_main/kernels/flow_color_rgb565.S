/*
 * flow_color_rgb565 -- map optical-flow direction codes to RGB565 bytes.
 *
 * Input:
 *   (a0) = 128x128 u8 flow code map from optical_flow.S:
 *          0 static, 50 right, 100 left, 150 down, 200 up
 *
 * Outputs:
 *   (a1) = RGB565 low-byte plane,  128x128 u8
 *   (a2) = RGB565 high-byte plane, 128x128 u8
 *
 * The display scanout buffer is interleaved RGB565, but this T1 fabric is
 * strongest at 128x128 e8 image planes. Producing two byte planes keeps the
 * kernel simple and lets the PS display path do only scale/copy interleave.
 */
.text
.global flow_color_rgb565_kernel
flow_color_rgb565_kernel:
    vle8.v      v8,  (a0)              /* flow code */
    vmv.v.i     v12, 0                 /* RGB565 low byte */
    vmv.v.i     v16, 0                 /* RGB565 high byte */

    /* 50: right -> red 0xf800: low=0x00 high=0xf8 */
    vmseq.vx    v0,  v8,  a3
    vmerge.vim  v16, v16, -8, v0

    /* 100: left -> cyan 0x07ff: low=0xff high=0x07 */
    vmseq.vx    v0,  v8,  a4
    vmerge.vim  v12, v12, -1, v0
    vmerge.vim  v16, v16, 7,  v0

    /* 150: down -> green 0x07e0: low=0xe0 high=0x07 */
    vmseq.vx    v0,  v8,  a5
    vmv.v.i     v20, 7
    vsll.vi     v20, v20, 5
    vmerge.vvm  v12, v12, v20, v0
    vmerge.vim  v16, v16, 7,   v0

    /* 200: up -> magenta 0xf81f: low=0x1f high=0xf8 */
    vmseq.vx    v0,  v8,  a6
    vmv.v.i     v20, -1
    vsrl.vi     v20, v20, 3
    vmerge.vvm  v12, v12, v20, v0
    vmerge.vim  v16, v16, -8,  v0

    vse8.v      v12, (a1)
    vse8.v      v16, (a2)
