/* Position-independent bridge copied into SceShellCore executable memory. */
#include "sm_shellcore_protocol_defs.h"

    .text
    .p2align 4
    .global sm_shellcore_bridge_blob_start
    .global sm_shellcore_bridge_exchange
    .global sm_shellcore_bridge_base_end
    .global sm_shellcore_bridge_blob_end
    .global sm_shellcore_bridge_launch_hook
    .global sm_shellcore_bridge_sandbox_hook
    .global sm_shellcore_bridge_sandbox_original
    .global sm_shellcore_bridge_install_all_hook
    .global sm_shellcore_bridge_launch_trampoline
    .global sm_shellcore_bridge_install_all_trampoline
    .global sm_shellcore_bridge_install_title_dir
    .global sm_shellcore_bridge_install_armed
    .global sm_shellcore_bridge_install_title_id_0
    .global sm_shellcore_bridge_install_title_id_1
    .global sm_shellcore_bridge_install_dir_0
    .global sm_shellcore_bridge_install_dir_1
    .global sm_shellcore_bridge_socket
    .global sm_shellcore_bridge_setsockopt
    .global sm_shellcore_bridge_connect
    .global sm_shellcore_bridge_read
    .global sm_shellcore_bridge_write
    .global sm_shellcore_bridge_close

/*
 * SceShellCore text is execute-only. The patched address must therefore be
 * fetched as part of a movabs instruction, never loaded from a data slot.
 */
    .macro bridge_call_import slot
    .byte 0x49, 0xbb               /* movabsq $address, %r11 */
\slot:
    .quad 0
    call *%r11
    .endm

    .macro bridge_load_qword slot, stack_offset
    .byte 0x48, 0xb8               /* movabsq $value, %rax */
\slot:
    .quad 0
    movq %rax, \stack_offset(%rsp)
    .endm

sm_shellcore_bridge_blob_start:
    /* Bridge ABI v4: framed requests and per-service session greeting. */
    .byte 0x66, 0x67, 0x90
sm_shellcore_bridge_launch_hook:
    pushq %rbx
    pushq %r12
    movq %rsi, %r12
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %r8
    pushq %r9
    subq $8, %rsp
    movl $SM_SHELLCORE_REQUEST_LAUNCH_VALUE, %edi
    /* RSI still contains title_id. */
    call sm_shellcore_bridge_exchange
    addq $8, %rsp
    popq %r9
    popq %r8
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    subq $8, %rsp
    call .Llaunch_trampoline
    addq $8, %rsp
    movq %rax, %rbx
    testl %eax, %eax
    jns 1f
    movl $SM_SHELLCORE_REQUEST_LAUNCH_FAILED_VALUE, %edi
    movq %r12, %rsi
    subq $8, %rsp
    call sm_shellcore_bridge_exchange
    addq $8, %rsp
1:
    movq %rbx, %rax
    popq %r12
    popq %rbx
    ret

    .p2align 4
sm_shellcore_bridge_sandbox_hook:
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %r8
    pushq %r9
    subq $8, %rsp
    movl $SM_SHELLCORE_REQUEST_SANDBOX_READY_VALUE, %edi
    xorl %esi, %esi
    call sm_shellcore_bridge_exchange
    addq $8, %rsp
    popq %r9
    popq %r8
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    .byte 0x49, 0xbb               /* movabsq $original, %r11 */
sm_shellcore_bridge_sandbox_original:
    .quad 0
    jmp *%r11

    .p2align 4
/* int exchange(uint16_t operation, const char *title_id) */
sm_shellcore_bridge_exchange:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    subq $184, %rsp

    /* Request [-112,-40), response [-144,-112), timeout [-160,-144). */
    pxor %xmm0, %xmm0
    leaq -112(%rbp), %rax
    movups %xmm0, 0(%rax)
    movups %xmm0, 16(%rax)
    movups %xmm0, 32(%rax)
    movups %xmm0, 48(%rax)
    movq %xmm0, 64(%rax)
    movl $SM_SHELLCORE_PROTOCOL_MAGIC, 0(%rax)
    movw $SM_SHELLCORE_PROTOCOL_VERSION, 4(%rax)
    movw %di, 6(%rax)
    movl $SM_SHELLCORE_REQUEST_SIZE, 36(%rax)
    testq %rsi, %rsi
    je 2f
    leaq SM_SHELLCORE_REQUEST_TITLE_OFFSET(%rax), %rdi
    pushq $(SM_SHELLCORE_REQUEST_TITLE_ID_SIZE - 1)
    popq %rcx
1:
    lodsb
    testb %al, %al
    je 2f
    stosb
    loop 1b
2:
    /* A nonce for this connection; never accept another transaction's reply. */
    rdtsc
    shlq $32, %rdx
    orq %rdx, %rax
    orq $1, %rax
    movq %rax, -96(%rbp)
    pushq $1
    popq %rdi
    pushq $1
    popq %rsi
    xorl %edx, %edx
    bridge_call_import sm_shellcore_bridge_socket
    testl %eax, %eax
    js 8f
    movl %eax, %ebx
    movq $SM_SHELLCORE_BRIDGE_IO_TIMEOUT_SEC, -160(%rbp)
    cmpw $SM_SHELLCORE_REQUEST_SANDBOX_READY_VALUE, -106(%rbp)
    jne 3f
    movb $SM_SHELLCORE_BRIDGE_SANDBOX_TIMEOUT_SEC, -160(%rbp)
3:
    andq $0, -152(%rbp)
    .byte 0x49, 0xbd
sm_shellcore_bridge_setsockopt:
    .quad 0
    movl $SM_SHELLCORE_BRIDGE_SO_SNDTIMEO, %r12d
.Ltimeouts:
    movl %ebx, %edi
    movl $SM_SHELLCORE_BRIDGE_SOL_SOCKET, %esi
    movl %r12d, %edx
    leaq -160(%rbp), %rcx
    pushq $16
    popq %r8
    call *%r13
    testl %eax, %eax
    jne 7f
    incl %r12d
    cmpl $(SM_SHELLCORE_BRIDGE_SO_RCVTIMEO + 1), %r12d
    jne .Ltimeouts

    movabsq $(0x65747379732f0100 | SM_SHELLCORE_BRIDGE_SOCKADDR_SIZE), %rax
    movq %rax, -192(%rbp)
    movabsq $0x68732f706d745f6d, %rax
    movq %rax, -184(%rbp)
    movabsq $0x6e756f6d776f6461, %rax
    movq %rax, -176(%rbp)
    movabsq $0x00006b636f732e74, %rax
    movq %rax, -168(%rbp)
    movl %ebx, %edi
    leaq -192(%rbp), %rsi
    pushq $SM_SHELLCORE_BRIDGE_SOCKADDR_SIZE
    popq %rdx
    bridge_call_import sm_shellcore_bridge_connect
    testl %eax, %eax
    jne 7f

    /* The server greets each connection with its current service session. */
    call .Lread_response
    testl %eax, %eax
    jne 7f
    movabsq $(SM_SHELLCORE_PROTOCOL_MAGIC | (SM_SHELLCORE_PROTOCOL_VERSION << 32)), %rax
    cmpq %rax, -144(%rbp)
    jne 7f
    cmpq $0, -128(%rbp)
    jne 7f
    cmpl $0, -120(%rbp)
    jne 7f
    movq -136(%rbp), %rax
    testq %rax, %rax
    je 7f
    movq %rax, -104(%rbp)

    leaq -112(%rbp), %r13
    pushq $SM_SHELLCORE_REQUEST_SIZE
    popq %r12
.Lwrite_request:
    movl %ebx, %edi
    movq %r13, %rsi
    movq %r12, %rdx
    bridge_call_import sm_shellcore_bridge_write
    testq %rax, %rax
    jle 7f
    cmpq %r12, %rax
    ja 7f
    addq %rax, %r13
    subq %rax, %r12
    jne .Lwrite_request
    call .Lread_response
    testl %eax, %eax
    jne 7f
    /* Echoed header, session and request nonce must all match. */
    leaq -112(%rbp), %rsi
    leaq -144(%rbp), %rdi
    pushq $3
    popq %rcx
    repe cmpsq
    jne 7f
    movl -120(%rbp), %r12d
    testl %r12d, %r12d
    jns 5f
7:
    pushq $SM_SHELLCORE_BRIDGE_IO_ERROR
    popq %r12
5:
    movl %ebx, %edi
    bridge_call_import sm_shellcore_bridge_close
    movl %r12d, %eax
    jmp 9f
8:
    orl $SM_SHELLCORE_BRIDGE_IO_ERROR, %eax
9:
    addq $184, %rsp
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.Lread_response:
    subq $8, %rsp
    leaq -144(%rbp), %r13
    pushq $SM_SHELLCORE_RESPONSE_SIZE
    popq %r12
.Lread_more:
    movl %ebx, %edi
    movq %r13, %rsi
    movq %r12, %rdx
    bridge_call_import sm_shellcore_bridge_read
    testq %rax, %rax
    jle .Lread_failed
    cmpq %r12, %rax
    ja .Lread_failed
    addq %rax, %r13
    subq %rax, %r12
    jne .Lread_more
    xorl %eax, %eax
    cmpl $SM_SHELLCORE_RESPONSE_SIZE, -116(%rbp)
    setne %al
    jmp .Lread_done
.Lread_failed:
    pushq $1
    popq %rax
.Lread_done:
    addq $8, %rsp
    ret

    /*
     * C replaces these reserved blocks with the copied prologue followed by
     * an absolute jump back into SceShellCore. Direct branches keep the
     * trampoline targets immutable after installation.
    */
    .p2align 4
.Llaunch_trampoline:
sm_shellcore_bridge_launch_trampoline:
    .fill SM_SHELLCORE_BRIDGE_TRAMPOLINE_SIZE, 1, 0x90
    /*
     * Firmware before 12.00 copies only this base. The optional install
     * compatibility bridge follows it in the allocated executable page.
     */
    .p2align 4
    .org sm_shellcore_bridge_blob_start + 0x400
sm_shellcore_bridge_base_end:

/*
 * FW 12+ removed the public AppInstallTitleDir RPC. AppInstallAll still
 * reaches this internal dispatcher, so one explicitly armed request is
 * redirected to the internal TitleDir implementation. Every ordinary
 * AppInstallAll call follows its original trampoline unchanged.
 */
sm_shellcore_bridge_install_all_hook:
    .byte 0xb0                    /* movb $armed, %al */
sm_shellcore_bridge_install_armed:
    .byte 0
    testb %al, %al
    je 1f

    /* Materialize both patched strings in readable stack memory. */
    subq $40, %rsp
    bridge_load_qword sm_shellcore_bridge_install_title_id_0, 0
    bridge_load_qword sm_shellcore_bridge_install_title_id_1, 8
    bridge_load_qword sm_shellcore_bridge_install_dir_0, 16
    bridge_load_qword sm_shellcore_bridge_install_dir_1, 24
    leaq 0(%rsp), %rdi
    leaq 16(%rsp), %rsi
    movl $1, %edx
    xorl %ecx, %ecx
    .byte 0x49, 0xbb             /* movabsq $address, %r11 */
sm_shellcore_bridge_install_title_dir:
    .quad 0
    call *%r11
    addq $40, %rsp
    ret
1:
    jmp .Linstall_all_trampoline

    .p2align 4
.Linstall_all_trampoline:
sm_shellcore_bridge_install_all_trampoline:
    .fill SM_SHELLCORE_BRIDGE_TRAMPOLINE_SIZE, 1, 0x90

    .p2align 3
    .org sm_shellcore_bridge_blob_start + 0x500
sm_shellcore_bridge_blob_end:
    .section .note.GNU-stack,"",@progbits
