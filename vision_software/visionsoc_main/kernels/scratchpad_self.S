.text
.global scratchpad_self_kernel
scratchpad_self_kernel:
    vle8.v  v8,  (a0)
    vse8.v  v8,  (a1)
    vle8.v  v12, (a1)
    vse8.v  v12, (a2)
