define i32 @choose(i32 %x) {
entry:
  switch i32 %x, label %default [
    i32 1, label %one
    i32 2, label %two
  ]
one:
  %a = add i32 %x, 10
  br label %one_more
one_more:
  %b = add i32 %a, 1
  br label %merge
two:
  %c = add i32 %x, 20
  br label %merge
default:
  %d = add i32 %x, 30
  br label %merge
merge:
  %r = phi i32 [ %b, %one_more ], [ %c, %two ], [ %d, %default ]
  ret i32 %r
}
