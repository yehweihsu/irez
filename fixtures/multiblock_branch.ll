define i32 @multi(i1 %cond, i32 %x) {
entry:
  br i1 %cond, label %then1, label %else

then1:
  %a = add i32 %x, 1
  br label %then2

then2:
  %b = mul i32 %a, 2
  br label %merge

else:
  %c = sub i32 %x, 1
  br label %merge

merge:
  %r = phi i32 [ %b, %then2 ], [ %c, %else ]
  ret i32 %r
}
