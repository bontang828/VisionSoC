.text
.global frame_passthrough_kernel
frame_passthrough_kernel:
    vle8.v  v8,  (a0)
    vse8.v  v8,  (a0)
