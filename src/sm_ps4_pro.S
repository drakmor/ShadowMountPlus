/* Validate the selected title, publish its clock flags, optionally force Pro. */
.intel_syntax noprefix
.text
.global sm_ps4_pro_start
sm_ps4_pro_start:
    push rbx
    push r12
    push rax
    mov rbx, rdi
    mov r12, rdx
    cmp byte ptr [rip+sm_ps4_pro_four_args], 0
    je 1f
    mov r12, rcx
1:
    call qword ptr [rip+sm_ps4_pro_original]
    mov r10, [rip+sm_ps4_pro_clock_policy]
    test rbx, rbx
    jz 3f
    cmp dword ptr [rbx], 0x5358504e  // NPXS system apps keep the game's policy
    je 4f
    test eax, eax
    js 3f
    test r12, r12
    jz 3f
    mov rdx, [rbx]
    cmp rdx, [rip+sm_ps4_pro_title]
    jne 3f
    mov dl, [rbx+8]
    cmp dl, [rip+sm_ps4_pro_title+8]
    jne 3f
    mov dl, [rip+sm_ps4_pro_policy]
    mov byte ptr [r10], dl
    test dl, 4
    je 4f
    mov ecx, [rip+sm_ps4_pro_attribute]
    or dword ptr [r12+rcx], 0x00800000
    jmp 4f
3:
    // Missing/mismatched IPC selection: don't inherit the previous game's boost.
    mov byte ptr [r10], 0
4:
    pop rcx
    pop r12
    pop rbx
    ret
.balign 8
.global sm_ps4_pro_original, sm_ps4_pro_policy, sm_ps4_pro_attribute
.global sm_ps4_pro_four_args, sm_ps4_pro_title, sm_ps4_pro_clock_policy
sm_ps4_pro_original: .quad 0
sm_ps4_pro_policy: .byte 0
sm_ps4_pro_attribute: .long 0
sm_ps4_pro_four_args: .byte 0
sm_ps4_pro_title: .zero 9
sm_ps4_pro_clock_policy: .quad 0
.global sm_ps4_pro_end
sm_ps4_pro_end:
.section .note.GNU-stack,"",@progbits
