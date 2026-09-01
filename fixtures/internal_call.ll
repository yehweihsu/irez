; ModuleID = 'internal_call'
source_filename = "internal_call.c"
target triple = "x86_64-unknown-linux-gnu"

define i32 @helper(i32 %x) {
entry:
  %y = add nsw i32 %x, 7
  ret i32 %y
}

define i32 @caller(i32 %x) {
entry:
  %r = call i32 @helper(i32 %x)
  ret i32 %r
}
