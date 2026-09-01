; Exercises the JSON boundary with a valid LLVM quoted identifier whose byte
; payload is not valid UTF-8 (ED A0 80 is the UTF-8 encoding of a surrogate).
define i32 @"unicode-\ED\A0\80"(i32 %x) {
entry:
  ret i32 %x
}
