/*
 * attention_self_transpose.S -- on-fabric 128x128 byte transpose, used by the
 * self-attention variant to turn a patch-token tile Q ([token][feat]) into the
 * Kt block ([feat][token] = K^T) the attention kernel V-loads. Self-attention
 * needs K = the live patches themselves, so Kt = transpose(Q); V = Q directly.
 *
 * Uses the proven transpose primitive (vertical load + horizontal store ->
 * memory receives M^T; see fyp_doc/2d_fabric_handoff.md 4.3). Bare 2-word
 * stream: libt1 supplies vtype/vl/vertical_mode/rs1 per issue.
 *
 *   word 0  vle8.v (vmode=1, vl=128)  : V-load src tile (transposed in VRF)
 *   word 1  vse8.v (vmode=0, vl=128)  : H-store -> dst = src^T in memory
 */
.text
.global attention_self_transpose
attention_self_transpose:
    vle8.v        v2,  (a0)              /* 0  V : load src (Q tile)            */
    vse8.v        v2,  (a0)              /* 1  H : store -> dst = src^T (Kt)     */
