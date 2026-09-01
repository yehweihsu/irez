; ModuleID = 'store_kernel'
source_filename = "store_kernel.c"
target triple = "x86_64-unknown-linux-gnu"

; Kernel-shaped function (XLA CPU style, backlog F2): the result leaves
; through a store and the return value is a constant null pointer, so
; trace-return sees no value evidence — trace-stores is the query that
; reaches the fmul -> fsub -> fdiv -> store chain.
define ptr @divide_bitcast_fusion(ptr %out, ptr %in) {
entry:
  %a = load float, ptr %in
  %b = fmul float %a, 2.0
  %c = fsub float %b, 1.0
  %d = fdiv float %c, 3.0
  store float %d, ptr %out
  ret ptr null
}
