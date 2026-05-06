.text
.global grid_vadd_kernel
grid_vadd_kernel:
    vle8.v  v8,  (a0)
    vle8.v  v12, (a1)
    vsub.vv v8,  v8, v12
    vse8.v  v8,  (a2)
