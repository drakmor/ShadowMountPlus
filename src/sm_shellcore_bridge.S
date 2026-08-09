/* Position-independent bridge copied into SceShellCore executable memory. */
    .text
    .p2align 4
    .global sm_shellcore_bridge_blob_start
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
    movl $1, %edi
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
    call *sm_shellcore_bridge_launch_trampoline(%rip)
    addq $8, %rsp
    movq %rax, %rbx
    testl %eax, %eax
    jns 1f
    movl $2, %edi
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
    jmp *sm_shellcore_bridge_install_all_trampoline(%rip)

/* int exchange(uint16_t operation, const char *title_id) */
    .p2align 4
sm_shellcore_bridge_exchange:
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    subq $160, %rsp

    /* Build the fixed 40-byte request. */
    movq $0, -64(%rbp)
    movq $0, -56(%rbp)
    movq $0, -48(%rbp)
    movq $0, -40(%rbp)
    movq $0, -32(%rbp)
    movl $0x534d5343, -64(%rbp)
    movw $1, -60(%rbp)
    movw %di, -58(%rbp)
    testq %rsi, %rsi
    je 2f
    leaq -56(%rbp), %rdi
    movl $31, %ecx
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

    /* connect(fd, &sockaddr, sockaddr.sun_len) */
    movl %ebx, %edi
    leaq sm_shellcore_bridge_sockaddr(%rip), %rsi
    movl $31, %edx
    call *sm_shellcore_bridge_connect(%rip)
    testl %eax, %eax
    jne 7f

    /* write(fd, &request, sizeof(request)) */
    movl %ebx, %edi
    leaq -64(%rbp), %rsi
    movl $40, %edx
    call *sm_shellcore_bridge_write(%rip)
    cmpq $40, %rax
    jne 7f

    /* Only status is needed by launch and launch-failure requests. */
    movl %ebx, %edi
    leaq -68(%rbp), %rsi
    movl $4, %edx
    call *sm_shellcore_bridge_read(%rip)
    cmpq $4, %rax
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

    .p2align 3
sm_shellcore_bridge_launch_trampoline:
    .quad 0
sm_shellcore_bridge_install_all_trampoline:
    .quad 0
sm_shellcore_bridge_install_title_dir:
    .quad 0
sm_shellcore_bridge_socket:
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
    .byte 31, 1
    .asciz "/system_tmp/shadowmount.sock"
    .zero 75
    .p2align 3
sm_shellcore_bridge_install_armed:
    .byte 0
sm_shellcore_bridge_install_title_id:
    .zero 32
sm_shellcore_bridge_install_dir:
    .zero 1024
sm_shellcore_bridge_blob_end:
    .section .note.GNU-stack,"",@progbits
