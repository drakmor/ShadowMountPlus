/* GPL-3.0-only. Wrap the BIG_APP call; cached and native profiles stay stock. */
.intel_syntax noprefix
.text
.global sm_ps4_clock_start
sm_ps4_clock_start:
    mov r9d, [rip+sm_ps4_clock_policy]
    test r9b, 3
    jz .Lstock
    mov rax, rsi
    cmp byte ptr [rip+sm_ps4_clock_three_args], 0
    je .Lprofile
    mov rax, rdx
.Lprofile:
    test rax, rax
    jz .Lstock
    cmp dword ptr [rax+8], 1
    ja .Lstock
    sub rsp, 72
    mov r10, rsi
    mov r11, rdi
    mov rsi, rax
    mov rdi, rsp
    mov ecx, 64
.global sm_ps4_clock_copy_size
.set sm_ps4_clock_copy_size, .-4
    rep movsb
    test r9b, 1
    jz .Lgpu
    lea rdi, [rsp+20]
    xor eax, eax
    mov ecx, 8
    rep stosd
.Lgpu:
    test r9b, 2
    jz .Lready
    mov dword ptr [rsp+12], 0
.global sm_ps4_clock_mode_offset
.set sm_ps4_clock_mode_offset, .-5
    mov eax, 2200
    test r9b, 16
    jz .Lmhz
    mov eax, 2350
.Lmhz:
    mov dword ptr [rsp+52], eax
.Lready:
    mov rdi, r11
    mov rsi, r10
    cmp byte ptr [rip+sm_ps4_clock_three_args], 0
    je .Ltwo
    mov rdx, rsp
    jmp .Lforward
.Ltwo:
    mov rsi, rsp
.Lforward:
    call qword ptr [rip+sm_ps4_clock_original]
    add rsp, 72
    ret
.Lstock:
    jmp qword ptr [rip+sm_ps4_clock_original]
.balign 8
.global sm_ps4_clock_original, sm_ps4_clock_policy
.global sm_ps4_clock_three_args, sm_ps4_clock_magic
sm_ps4_clock_original: .quad 0
sm_ps4_clock_policy: .quad 0
sm_ps4_clock_three_args: .byte 0
sm_ps4_clock_magic: .ascii "SMCLK003"
.global sm_ps4_clock_end
sm_ps4_clock_end:
.section .note.GNU-stack,"",@progbits
