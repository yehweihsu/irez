define i32 @loop(i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %test = icmp slt i32 %i, %n
  br i1 %test, label %body, label %exit
body:
  %next = add i32 %i, 1
  br label %header
exit:
  ret i32 %i
}

define i32 @multi_exit(i1 %c) {
entry:
  br i1 %c, label %yes, label %no
yes:
  ret i32 1
no:
  ret i32 0
}

define void @infinite(i1 %c) {
entry:
  br i1 %c, label %spin, label %done
spin:
  br label %spin
done:
  ret void
dead:
  unreachable
}
