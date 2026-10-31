/* Self-contained game wrappers. Request 120 Hz only after support query succeeds. */
.intel_syntax noprefix
.text
.global sm_ps4_video_start, sm_ps4_video_open, sm_ps4_video_buffers
sm_ps4_video_start:
sm_ps4_video_open:
    push rbx
    mov r11, qword ptr [rip+sm_ps4_video_functions]
    call qword ptr [r11]
    mov ebx, eax
    test eax, eax
    js .Lopen_return
    mov edi, eax
    call .Lset120
.Lopen_return:
    mov eax, ebx
    pop rbx
    ret
sm_ps4_video_buffers:
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    call .Lset120
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    mov r11, qword ptr [rip+sm_ps4_video_functions]
    jmp qword ptr [r11+8]
.Lset120:
    push rbx
    mov ebx, edi
    mov esi, 15
    xor edx, edx
    xor ecx, ecx
    xor r8d, r8d
    mov r11, qword ptr [rip+sm_ps4_video_functions]
    call qword ptr [r11+16]
    mov r11, [rip+sm_ps4_video_result]
    mov [r11], eax
    mov [r11+12], ebx
    test eax, eax
    jle .Lunsupported
    mov edi, ebx
    mov esi, 15
    xor edx, edx
    xor ecx, ecx
    xor r8d, r8d
    mov r11, qword ptr [rip+sm_ps4_video_functions]
    call qword ptr [r11+24]
    mov r11, [rip+sm_ps4_video_result]
    mov [r11+4], eax
    mov byte ptr [r11+8], 2
    pop rbx
    ret
.Lunsupported:
    mov byte ptr [r11+8], 1
    pop rbx
    ret
.balign 8
.global sm_ps4_video_functions, sm_ps4_video_result
sm_ps4_video_functions: .quad 0
sm_ps4_video_result: .quad 0
.global sm_ps4_video_end
sm_ps4_video_end:
.section .note.GNU-stack,"",@progbits
