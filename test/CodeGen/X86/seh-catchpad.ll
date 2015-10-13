; RUN: llc < %s | FileCheck %s

; Based on the source:
; extern "C" int puts(const char *);
; extern "C" int printf(const char *, ...);
; extern "C" int do_div(int a, int b) { return a / b; }
; extern "C" int filt();
; int main() {
;   __try {
;     __try {
;       do_div(1, 0);
;     } __except (1) {
;       __try {
;         do_div(1, 0);
;       } __finally {
;         puts("finally");
;       }
;     }
;   } __except (filt()) {
;     puts("caught");
;   }
;   return 0;
; }

; ModuleID = 't.cpp'
target datalayout = "e-m:w-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

$"\01??_C@_07MKBLAIAL@finally?$AA@" = comdat any

$"\01??_C@_06IBDBCMGJ@caught?$AA@" = comdat any

@"\01??_C@_07MKBLAIAL@finally?$AA@" = linkonce_odr unnamed_addr constant [8 x i8] c"finally\00", comdat, align 1
@"\01??_C@_06IBDBCMGJ@caught?$AA@" = linkonce_odr unnamed_addr constant [7 x i8] c"caught\00", comdat, align 1

; Function Attrs: nounwind readnone
define i32 @do_div(i32 %a, i32 %b) #0 {
entry:
  %div = sdiv i32 %a, %b
  ret i32 %div
}

define i32 @main() #1 personality i8* bitcast (i32 (...)* @__C_specific_handler to i8*) {
entry:
  %call = invoke i32 @do_div(i32 1, i32 0) #4
          to label %__try.cont.12 unwind label %catch.dispatch

catch.dispatch:                                   ; preds = %entry
  %0 = catchpad [i8* null]
          to label %__except unwind label %catchendblock

__except:                                         ; preds = %catch.dispatch
  catchret %0 to label %__except.2

__except.2:                                       ; preds = %__except
  %call4 = invoke i32 @do_div(i32 1, i32 0) #4
          to label %invoke.cont.3 unwind label %ehcleanup

invoke.cont.3:                                    ; preds = %__except.2
  invoke fastcc void @"\01?fin$0@0@main@@"() #4
          to label %__try.cont.12 unwind label %catch.dispatch.7

catchendblock:                                    ; preds = %catch.dispatch
  catchendpad unwind label %catch.dispatch.7

ehcleanup:                                        ; preds = %__except.2
  %1 = cleanuppad []
  invoke fastcc void @"\01?fin$0@0@main@@"() #4
          to label %invoke.cont.6 unwind label %ehcleanup.end

invoke.cont.6:                                    ; preds = %ehcleanup
  cleanupret %1 unwind label %catch.dispatch.7

catch.dispatch.7:                                 ; preds = %invoke.cont.3, %invoke.cont.6, %ehcleanup.end, %catchendblock
  %2 = catchpad [i8* bitcast (i32 (i8*, i8*)* @"\01?filt$0@0@main@@" to i8*)]
          to label %__except.ret unwind label %catchendblock.8

__except.ret:                                     ; preds = %catch.dispatch.7
  catchret %2 to label %__except.9

__except.9:                                       ; preds = %__except.ret
  %call11 = tail call i32 @puts(i8* nonnull getelementptr inbounds ([7 x i8], [7 x i8]* @"\01??_C@_06IBDBCMGJ@caught?$AA@", i64 0, i64 0))
  br label %__try.cont.12

__try.cont.12:                                    ; preds = %invoke.cont.3, %entry, %__except.9
  ret i32 0

catchendblock.8:                                  ; preds = %catch.dispatch.7
  catchendpad unwind to caller

ehcleanup.end:                                    ; preds = %ehcleanup
  cleanupendpad %1 unwind label %catch.dispatch.7
}

; CHECK: main:                                   # @main
; CHECK: .seh_proc main
; CHECK:         .seh_handler __C_specific_handler, @unwind, @except
; CHECK:         pushq   %rbp
; CHECK:         .seh_pushreg 5
; CHECK:         subq    $48, %rsp
; CHECK:         .seh_stackalloc 48
; CHECK:         leaq    48(%rsp), %rbp
; CHECK:         .seh_setframe 5, 48
; CHECK:         .seh_endprologue
; CHECK: .Ltmp0:
; CHECK:         movl    $1, %ecx
; CHECK:         xorl    %edx, %edx
; CHECK:         callq   do_div
; CHECK: .Ltmp1:
; CHECK: .LBB1_[[epilogue:[0-9]+]]:                                # %__try.cont.12
; CHECK:         xorl    %eax, %eax
; CHECK:         addq    $48, %rsp
; CHECK:         popq    %rbp
; CHECK:         retq
; CHECK: .LBB1_[[except1bb:[0-9]+]]:                                # %__except
; CHECK: .Ltmp2:
; CHECK:         movl    $1, %ecx
; CHECK:         xorl    %edx, %edx
; CHECK:         callq   do_div
; CHECK: .Ltmp3:
; CHECK:         callq   "?fin$0@0@main@@"
; CHECK:         jmp     .LBB1_[[epilogue]]
; CHECK: .LBB1_[[except2bb:[0-9]+]]:                                # %__except.ret
; CHECK:         leaq    "??_C@_06IBDBCMGJ@caught?$AA@"(%rip), %rcx
; CHECK:         callq   puts
; CHECK:         jmp     .LBB1_[[epilogue]]

; CHECK:         .seh_handlerdata
; CHECK-NEXT:         .long   (.Ltmp14-.Ltmp13)/16
; CHECK-NEXT: .Ltmp13:
; CHECK-NEXT:         .long   .Ltmp0@IMGREL
; CHECK-NEXT:         .long   .Ltmp1@IMGREL+1
; CHECK-NEXT:         .long   1
; CHECK-NEXT:         .long   .LBB1_[[except1bb]]@IMGREL
; CHECK-NEXT:         .long   .Ltmp0@IMGREL
; CHECK-NEXT:         .long   .Ltmp1@IMGREL+1
; CHECK-NEXT:         .long   "?filt$0@0@main@@"@IMGREL
; CHECK-NEXT:         .long   .LBB1_[[except2bb]]@IMGREL
; CHECK-NEXT:         .long   .Ltmp2@IMGREL
; CHECK-NEXT:         .long   .Ltmp3@IMGREL+1
; CHECK-NEXT:         .long   .LBB1_[[finbb:[0-9]+]]@IMGREL
; CHECK-NEXT:         .long   0
; CHECK-NEXT:         .long   .Ltmp2@IMGREL
; CHECK-NEXT:         .long   .Ltmp3@IMGREL+1
; CHECK-NEXT:         .long   "?filt$0@0@main@@"@IMGREL
; CHECK-NEXT:         .long   .LBB1_6@IMGREL
; CHECK-NEXT: .Ltmp14:

; CHECK:         .text
; CHECK:         .seh_endproc

; CHECK: "?dtor$4@?0?main@4HA":
; CHECK: .seh_proc "?dtor$4@?0?main@4HA"
; CHECK:         .seh_handler __C_specific_handler, @unwind, @except
; CHECK: .LBB1_[[finbb]]:                                # %ehcleanup
; CHECK:         movq    %rdx, 16(%rsp)
; CHECK:         pushq   %rbp
; CHECK:         .seh_pushreg 5
; CHECK:         subq    $32, %rsp
; CHECK:         .seh_stackalloc 32
; CHECK:         leaq    48(%rdx), %rbp
; CHECK:         .seh_endprologue
; CHECK:         callq   "?fin$0@0@main@@"
; CHECK:         nop
; CHECK:         addq    $32, %rsp
; CHECK:         popq    %rbp
; CHECK:         retq
; CHECK:         .seh_handlerdata
; CHECK:         .seh_endproc

define internal i32 @"\01?filt$0@0@main@@"(i8* nocapture readnone %exception_pointers, i8* nocapture readnone %frame_pointer) #1 {
entry:
  %call = tail call i32 @filt()
  ret i32 %call
}

; CHECK: "?filt$0@0@main@@":                     # @"\01?filt$0@0@main@@"
; CHECK: .seh_proc "?filt$0@0@main@@"
; CHECK:         .seh_endprologue
; CHECK:         rex64 jmp       filt  # TAILCALL
; CHECK:         .seh_handlerdata

declare i32 @filt() #1

declare i32 @__C_specific_handler(...)

; Function Attrs: noinline nounwind
define internal fastcc void @"\01?fin$0@0@main@@"() #2 {
entry:
  %call = tail call i32 @puts(i8* getelementptr inbounds ([8 x i8], [8 x i8]* @"\01??_C@_07MKBLAIAL@finally?$AA@", i64 0, i64 0)) #5
  ret void
}

; Function Attrs: nounwind
declare i32 @puts(i8* nocapture readonly) #3

attributes #0 = { nounwind readnone "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "target-features"="+sse,+sse2" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "target-features"="+sse,+sse2" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #2 = { noinline nounwind "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "target-features"="+sse,+sse2" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #3 = { nounwind "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "target-features"="+sse,+sse2" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #4 = { noinline }
attributes #5 = { nounwind }
