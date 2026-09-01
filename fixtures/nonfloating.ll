; ModuleID = 'nonfloating'
source_filename = "nonfloating.c"
target triple = "x86_64-unknown-linux-gnu"

declare i32 @external(i32)

define i32 @choose(i32 %x, i1 %cond) !dbg !5 {
entry:
  br i1 %cond, label %yes, label %no, !dbg !10
yes:
  %a = add nsw i32 %x, 1, !dbg !11
  br label %merge, !dbg !12
no:
  %b = sub nsw i32 %x, 1, !dbg !13
  br label %merge, !dbg !14
merge:
  %v = phi i32 [ %a, %yes ], [ %b, %no ], !dbg !15
  %r = call i32 @external(i32 %v), !dbg !16
  ret i32 %r, !dbg !17
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}
!llvm.ident = !{!4}
!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "irez fixture", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "nonfloating.c", directory: "/fixtures")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{!"irez"}
!5 = distinct !DISubprogram(name: "choose", scope: !1, file: !1, line: 1, type: !6, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!6 = !DISubroutineType(types: !7)
!7 = !{!8, !8, !9}
!8 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!9 = !DIBasicType(name: "_Bool", size: 8, encoding: DW_ATE_boolean)
!10 = !DILocation(line: 2, column: 3, scope: !5)
!11 = !DILocation(line: 3, column: 5, scope: !5)
!12 = !DILocation(line: 4, column: 3, scope: !5)
!13 = !DILocation(line: 5, column: 5, scope: !5)
!14 = !DILocation(line: 6, column: 3, scope: !5)
!15 = !DILocation(line: 7, column: 3, scope: !5)
!16 = !DILocation(line: 8, column: 10, scope: !5)
!17 = !DILocation(line: 9, column: 3, scope: !5)
