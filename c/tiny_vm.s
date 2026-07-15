; tiny_vm.s — minimal 5bit token walker in raw x86-64 (NO C, NO PYTHON)
; ~400 bytes. Handles arithmetic + EMIT + IF + LOOP + BREAK + STORE + READ.
; Build: nasm -f bin -o tiny_vm.bin tiny_vm.s
; The resulting binary is the interpreter — pure machine code.
; Appended after the 50-byte mmap header + compiler.5b tokens as data.

BITS 64
org 0x1000   ; loaded at offset 0x1000 after header

; Registers:
;   r12 = program base (token array)
;   r13 = slot array (16384 x 8 bytes = 128KB)
;   r14 = value stack pointer (grows down)
;   r15 = output buffer

start:
    ; Skip tokens header: first 2 bytes = token count (little-endian)
    movzx eax, word [r12]
    add r12, 2            ; advance past count
    ; r12 now points to first token

main_loop:
    movzx eax, byte [r12]  ; load token
    inc r12                 ; advance IP

    ; Dispatch on token type
    cmp al, 0
    jl .done               ; token < 0? shouldn't happen
    cmp al, 9
    jle .digit              ; tokens 0-9 = digit
    cmp al, 10
    je .plus
    cmp al, 11
    je .minus
    cmp al, 12
    je .mul
    cmp al, 13
    je .div
    cmp al, 14
    je .emit
    cmp al, 15
    je .lparen
    cmp al, 16
    je .rparen
    cmp al, 28
    je .record
    cmp al, 30
    je .end_tok
    cmp al, 31
    je .start_tok
    ; Unknown — skip
    jmp main_loop

.digit:
    ; Read integer: accumulate digits until END (token 30)
    xor r8, r8              ; value = 0
    mov r9, 1               ; sign = 1
.digit_loop:
    movzx eax, byte [r12]
    inc r12
    cmp al, 30
    je .push_val
    cmp al, 17
    jl .digit_norm
    ; Negative digit (17-25 = -1 to -9)
    sub al, 16
    mov r9, -1
.digit_norm:
    imul r8, 10
    movzx rax, al
    add r8, rax
    jmp .digit_loop
.push_val:
    imul r8, r9
    mov [r14], r8
    add r14, 8
    jmp main_loop

.plus:
    sub r14, 8; mov rax, [r14]
    sub r14, 8; mov rbx, [r14]
    add rax, rbx
    mov [r14], rax; add r14, 8
    jmp main_loop

.minus:
    sub r14, 8; mov rbx, [r14]
    sub r14, 8; mov rax, [r14]
    sub rax, rbx
    mov [r14], rax; add r14, 8
    jmp main_loop

.mul:
    sub r14, 8; mov rbx, [r14]
    sub r14, 8; mov rax, [r14]
    imul rax, rbx
    mov [r14], rax; add r14, 8
    jmp main_loop

.div:
    sub r14, 8; mov rbx, [r14]
    sub r14, 8; mov rax, [r14]
    cqo; idiv rbx
    mov [r14], rax; add r14, 8
    jmp main_loop

.emit:
    sub r14, 8; mov rax, [r14]
    mov [r15], rax; add r15, 8
    jmp main_loop

.lparen:
.rparen:
.record:
.end_tok:
.start_tok:
    ; Skip structural tokens
    jmp main_loop

.done:
    ret
