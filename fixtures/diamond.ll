; ModuleID = 'diamond'
source_filename = "diamond.c"
target triple = "x86_64-unknown-linux-gnu"

; %y is reachable from %a both directly (operand 1) and through %b.
; A backward slice from ret with --budget-depth 2 reaches %y at maximum
; depth through %b while %y is already known: that must not be reported as
; a truncation boundary (V00_00 bug B4).
define i32 @diamond(i32 %x, i32 %y) {
entry:
  %b = add nsw i32 %y, 1
  %a = add nsw i32 %b, %y
  ret i32 %a
}
