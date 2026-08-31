/* Position-independent bridge copied into SceShellCore executable memory. */
#include "sm_shellcore_protocol_defs.h"

    .text
    .p2align 4
    .global sm_shellcore_bridge_blob_start
    .global sm_shellcore_bridge_code_end
    .global sm_shellcore_bridge_blob_end
    .global sm_shellcore_bridge_launch_hook
    .global sm_shellcore_bridge_install_all_hook
    .global sm_shellcore_bridge_launch_trampoline
    .global sm_shellcore_bridge_install_all_trampoline
    .global sm_shellcore_bridge_install_title_dir
    .global sm_shellcore_bridge_install_armed
    .global sm_shellcore_bridge_install_title_id
    .global sm_shellcore_bridge_install_dir
    .global sm_shellcore_bridge_socket
    .global sm_shellcore_bridge_setsockopt
    .global sm_shellcore_bridge_connect
    .global sm_shellcore_bridge_read
    .global sm_shellcore_bridge_write
    .global sm_shellcore_bridge_close

sm_shellcore_bridge_blob_start:
sm_shellcore_bridge_launch_hook:
    pushq %rbx
    pushq %r12
    movq %rsi, %r12
    pushfq
    pushq %rax
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    subq $8, %rsp
    movl $SM_SHELLCORE_REQUEST_LAUNCH_VALUE, %edi
    /* RSI still contains title_id. */
    call sm_shellcore_bridge_exchange
    addq $8, %rsp
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax
    popfq
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

/*
 * FW 12+ removed the public AppInstallTitleDir RPC. AppInstallAll still
 * reaches this internal dispatcher, so one explicitly armed request is
 * redirected to the internal TitleDir implementation. Every ordinary
 * AppInstallAll call follows its original trampoline unchanged.
 */
    .p2align 4
sm_shellcore_bridge_install_all_hook:
    cmpb $0, sm_shellcore_bridge_install_armed(%rip)
    je 1f
    movb $0, sm_shellcore_bridge_install_armed(%rip)
    leaq sm_shellcore_bridge_install_title_id(%rip), %rdi
    leaq sm_shellcore_bridge_install_dir(%rip), %rsi
    movl $1, %edx
    xorl %ecx, %ecx
    jmp *sm_shellcore_bridge_install_title_dir(%rip)
1:
    jmp .Linstall_all_trampoline

/* int exchange(uint16_t operation, const char *title_id) */
    .p2align 4
sm_shellcore_bridge_exchange:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    subq $160, %rsp

    /* Build the fixed-size request. */
    movq $0, -64(%rbp)
    movq $0, -56(%rbp)
    movq $0, -48(%rbp)
    movq $0, -40(%rbp)
    movq $0, -32(%rbp)
    movl $SM_SHELLCORE_PROTOCOL_MAGIC, -64(%rbp)
    movw $SM_SHELLCORE_PROTOCOL_VERSION, -60(%rbp)
    movw %di, -58(%rbp)
    testq %rsi, %rsi
    je 2f
    leaq -56(%rbp), %rdi
    movl $(SM_SHELLCORE_REQUEST_TITLE_ID_SIZE - 1), %ecx
1:
    movb (%rsi), %al
    testb %al, %al
    je 2f
    movb %al, (%rdi)
    incq %rsi
    incq %rdi
    decl %ecx
    jne 1b
2:
    /* Use libkernel_sys wrappers. Direct syscalls from SceShellCore are fatal. */
    movl $1, %edi
    movl $1, %esi
    xorl %edx, %edx
    call *sm_shellcore_bridge_socket(%rip)
    testl %eax, %eax
    js 8f
    movl %eax, %ebx

    /*
     * The mount preparation service is intentionally synchronous, but a
     * stalled payload must not hold SceLncServiceIpcJobQueue indefinitely.
    */
    movq $SM_SHELLCORE_BRIDGE_IO_TIMEOUT_SEC, -96(%rbp)
    movq $0, -88(%rbp)
    movl %ebx, %edi
    movl $SM_SHELLCORE_BRIDGE_SOL_SOCKET, %esi
    movl $SM_SHELLCORE_BRIDGE_SO_SNDTIMEO, %edx
    leaq -96(%rbp), %rcx
    movl $SM_SHELLCORE_BRIDGE_TIMEVAL_SIZE, %r8d
    call *sm_shellcore_bridge_setsockopt(%rip)
    testl %eax, %eax
    jne 7f
    movl %ebx, %edi
    movl $SM_SHELLCORE_BRIDGE_SOL_SOCKET, %esi
    movl $SM_SHELLCORE_BRIDGE_SO_RCVTIMEO, %edx
    leaq -96(%rbp), %rcx
    movl $SM_SHELLCORE_BRIDGE_TIMEVAL_SIZE, %r8d
    call *sm_shellcore_bridge_setsockopt(%rip)
    testl %eax, %eax
    jne 7f

    /* connect(fd, &sockaddr, sockaddr.sun_len) */
    movl %ebx, %edi
    leaq sm_shellcore_bridge_sockaddr(%rip), %rsi
    movzbl sm_shellcore_bridge_sockaddr(%rip), %edx
    call *sm_shellcore_bridge_connect(%rip)
    testl %eax, %eax
    jne 7f

    /* write(fd, &request, sizeof(request)) */
    movl %ebx, %edi
    leaq -64(%rbp), %rsi
    movl $SM_SHELLCORE_REQUEST_SIZE, %edx
    call *sm_shellcore_bridge_write(%rip)
    cmpq $SM_SHELLCORE_REQUEST_SIZE, %rax
    jne 7f

    /* Only status is needed by launch and launch-failure requests. */
    movl %ebx, %edi
    leaq -68(%rbp), %rsi
    movl $SM_SHELLCORE_RESPONSE_SIZE, %edx
    call *sm_shellcore_bridge_read(%rip)
    cmpq $SM_SHELLCORE_RESPONSE_SIZE, %rax
    jne 7f
    movl -68(%rbp), %r12d
    jmp 5f
7:
    movl $5, %r12d
5:
    movl %ebx, %edi
    call *sm_shellcore_bridge_close(%rip)
    movl %r12d, %eax
    jmp 9f
8:
    movl $5, %eax
9:
    addq $160, %rsp
    popq %r12
    popq %rbx
    popq %rbp
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
    .p2align 4
.Linstall_all_trampoline:
sm_shellcore_bridge_install_all_trampoline:
    .fill SM_SHELLCORE_BRIDGE_TRAMPOLINE_SIZE, 1, 0x90

    .p2align 3
sm_shellcore_bridge_install_title_dir:
    .quad 0
sm_shellcore_bridge_socket:
    .quad 0
sm_shellcore_bridge_setsockopt:
    .quad 0
sm_shellcore_bridge_connect:
    .quad 0
sm_shellcore_bridge_read:
    .quad 0
sm_shellcore_bridge_write:
    .quad 0
sm_shellcore_bridge_close:
    .quad 0
sm_shellcore_bridge_sockaddr:
    .byte sm_shellcore_bridge_sockaddr_end - sm_shellcore_bridge_sockaddr, 1
    .asciz SM_SHELLCORE_SOCKET_PATH
sm_shellcore_bridge_sockaddr_end:

    /*
     * PS5 user pages are 16 KiB. Keep all executable code and immutable
     * imports in the first page, and the FW 12+ mutable install request in a
     * separate non-executable page.
    */
    .p2align 14, 0
sm_shellcore_bridge_code_end:
sm_shellcore_bridge_install_armed:
    .byte 0
sm_shellcore_bridge_install_title_id:
    .zero SM_SHELLCORE_REQUEST_TITLE_ID_SIZE
sm_shellcore_bridge_install_dir:
    .zero SM_SHELLCORE_BRIDGE_INSTALL_DIR_SIZE
sm_shellcore_bridge_blob_end:
    .section .note.GNU-stack,"",@progbits
