; ModuleID = 'globals'
source_filename = "globals.c"
target triple = "x86_64-unknown-linux-gnu"

@g = global i32 0

; @g is referenced from two functions. Its entity must stay module-scoped:
; function_id must remain NULL no matter which function is materialized
; (V00_00 bug B6).
define i32 @reader(i32 %x) {
entry:
  %v = load i32, ptr @g
  %r = add nsw i32 %v, %x
  ret i32 %r
}

define i32 @writer(i32 %x) {
entry:
  store i32 %x, ptr @g
  ret i32 %x
}
