/*
 * frame_passthrough -- load Y plane from (a0) and store it to (a1) unchanged.
 * Single LSU round-trip, no compute. Used as the "colour camera passthrough"
 * baseline; lets the rest of the pipeline (camera capture, NV12 assembly, DP
 * scanout) be verified independently of any T1 compute.
 *
 * libt1 conventions handled by active_kernel_dispatcher.h:
 *   (a0) = source PA, (a1) = destination PA; H mode (default).
 */
.text
.global frame_passthrough_kernel
frame_passthrough_kernel:
    vle8.v v8, (a0)
    vse8.v v8, (a1)
