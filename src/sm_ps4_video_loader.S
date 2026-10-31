#include "sm_shellcore_protocol_defs.h"
#include "sm_ps4_video_defs.h"

    .text
    .p2align 4
    .global sm_ps4_video_loader_start
    .global sm_ps4_video_loader_end
    .global sm_ps4_video_loader_dlsym
    .global sm_ps4_video_loader_exchange
    .global sm_ps4_video_loader_pid
    .global sm_ps4_video_loader_once
    .global sm_ps4_video_loader_open
    .global sm_ps4_video_loader_buffers

/* int sceKernelDlsym(handle, name, address_out). */
sm_ps4_video_loader_start:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $40, %rsp
    movl %edi, %r12d
    movq %rsi, %r13
    movq %rdx, %r14
    .byte 0x49, 0xbb             /* movabsq $original, %r11 */
sm_ps4_video_loader_dlsym:
    .quad 0
    movq %r11, 8(%rsp)
    call *%r11
    movl %eax, %ebx
    testl %eax, %eax
    jne .Ldone
    testq %r13, %r13
    je .Ldone
    testq %r14, %r14
    je .Ldone
    /* Read names one byte at a time: short names can end at a page edge. */
    .macro match_byte value, offset, fail
    cmpb $\value, \offset(%r13)
    jne \fail
    .endm
    match_byte 0x73, 0, .Ldone
    match_byte 0x63, 1, .Ldone
    match_byte 0x65, 2, .Ldone
    match_byte 0x56, 3, .Ldone
    match_byte 0x69, 4, .Ldone
    match_byte 0x64, 5, .Ldone
    match_byte 0x65, 6, .Ldone
    match_byte 0x6f, 7, .Ldone
    match_byte 0x4f, 8, .Ldone
    match_byte 0x75, 9, .Ldone
    match_byte 0x74, 10, .Ldone
    cmpb $0x4f, 11(%r13)
    je .Lname_open
    cmpb $0x52, 11(%r13)
    jne .Ldone
    match_byte 0x65, 12, .Ldone
    match_byte 0x67, 13, .Ldone
    match_byte 0x69, 14, .Ldone
    match_byte 0x73, 15, .Ldone
    match_byte 0x74, 16, .Ldone
    match_byte 0x65, 17, .Ldone
    match_byte 0x72, 18, .Ldone
    match_byte 0x42, 19, .Ldone
    match_byte 0x75, 20, .Ldone
    match_byte 0x66, 21, .Ldone
    match_byte 0x66, 22, .Ldone
    match_byte 0x65, 23, .Ldone
    match_byte 0x72, 24, .Ldone
    match_byte 0x73, 25, .Ldone
    match_byte 0, 26, .Ldone
    movl $1, %r15d
    jmp .Lmatched
.Lname_open:
    match_byte 0x70, 12, .Ldone
    match_byte 0x65, 13, .Ldone
    match_byte 0x6e, 14, .Ldone
    match_byte 0, 15, .Ldone
    xorl %r15d, %r15d
.Lmatched:
    .byte 0x49, 0xbb             /* movabsq $one_shot_flag, %r11 */
sm_ps4_video_loader_once:
    .quad 0
    movq %r11, %r13
    movq (%r14), %rax
    testq %rax, %rax
    je .Lsignal
    movq %rax, SM_PS4_VIDEO_LOADER_FUNCTIONS_OFFSET(%r13,%r15,8)
    cmpq $0, SM_PS4_VIDEO_LOADER_FUNCTIONS_OFFSET+16(%r13)
    je .Lresolve_helpers
    cmpq $0, SM_PS4_VIDEO_LOADER_FUNCTIONS_OFFSET+24(%r13)
    jne .Lhelpers_ready
.Lresolve_helpers:

    movl %r12d, %edi
    leaq .Lsupport_name(%rip), %rsi
    leaq 0(%rsp), %rdx
    movq $0, 0(%rsp)
    movq 8(%rsp), %r11
    call *%r11
    testl %eax, %eax
    jne .Lsignal
    movq 0(%rsp), %rax
    testq %rax, %rax
    je .Lsignal
    movq %rax, SM_PS4_VIDEO_LOADER_FUNCTIONS_OFFSET+16(%r13)

    movl %r12d, %edi
    leaq .Lconfigure_name(%rip), %rsi
    leaq 0(%rsp), %rdx
    movq $0, 0(%rsp)
    movq 8(%rsp), %r11
    call *%r11
    testl %eax, %eax
    jne .Lsignal
    movq 0(%rsp), %rax
    testq %rax, %rax
    je .Lsignal
    movq %rax, SM_PS4_VIDEO_LOADER_FUNCTIONS_OFFSET+24(%r13)
.Lhelpers_ready:
    testl %r15d, %r15d
    jne .Lreplace_buffers
    .byte 0x49, 0xbb             /* movabsq $open_wrapper, %r11 */
sm_ps4_video_loader_open:
    .quad 0
    jmp .Lreplace
.Lreplace_buffers:
    .byte 0x49, 0xbb             /* movabsq $buffers_wrapper, %r11 */
sm_ps4_video_loader_buffers:
    .quad 0
.Lreplace:
    movq %r11, (%r14)
.Lsignal:
    lock btsl $0, (%r13)
    jc .Ldone
    movl $SM_SHELLCORE_REQUEST_VIDEO_READY_VALUE, %edi
    xorl %esi, %esi
    xorl %edx, %edx
    .byte 0xb9                   /* movl $pid, %ecx */
sm_ps4_video_loader_pid:
    .long 0
    movl %r12d, %r8d
    .byte 0x49, 0xbb             /* movabsq $exchange, %r11 */
sm_ps4_video_loader_exchange:
    .quad 0
    call *%r11
.Ldone:
    movl %ebx, %eax
    addq $40, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.Lsupport_name:
    .asciz "sceVideoOutIsOutputSupported"
.Lconfigure_name:
    .asciz "sceVideoOutConfigureOutput"
sm_ps4_video_loader_end:
    .section .note.GNU-stack,"",@progbits
