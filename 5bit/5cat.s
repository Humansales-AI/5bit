.globl _main
.text
_main:
    pushq %rbp; movq %rsp, %rbp
    pushq %r12; pushq %r13; pushq %r14; pushq %r15; pushq %rbx
    movq %rdi, %r12; movq %rsi, %r13
    cmpq $2, %r12; jl usage

    movq 8(%r13), %rdi
    cmpb $99, (%rdi); jne run_mode
    cmpq $4, %r12; jl usage
    movq 16(%r13), %rdi; movl $0x601, %esi; movl $0x1B6, %edx; callq _open
    testq %rax, %rax; js err; movq %rax, %r14
    movb $0, Lbuf(%rip)
    leaq Lbuf(%rip), %rsi; movq %r14, %rdi; movl $1, %edx; callq _write
    
    movl $3, %r15d
.LcatLoop:
    cmpq %r12, %r15; jge .LcatDone
    movq (%r13,%r15,8), %rbx
    
    movzbl (%rbx), %eax
    cmpb $83, %al; jne .TryE
    cmpb $84, 1(%rbx); jne .TryS2
    cmpb $65, 2(%rbx); jne .TryS2
    cmpb $82, 3(%rbx); jne .TryS2
    cmpb $84, 4(%rbx); jne .TryS2
    movl $31, %eax; jmp .LcatEmit
.TryS2:
    cmpb $84, 1(%rbx); jne .LcatNum
    cmpb $79, 2(%rbx); jne .LcatNum
    cmpb $82, 3(%rbx); jne .LcatNum
    cmpb $69, 4(%rbx); jne .LcatNum
    movl $12, %eax; jmp .LcatEmit

.TryE:
    cmpb $69, %al; jne .TryP
    cmpb $78, 1(%rbx); jne .LcatNum
    cmpb $68, 2(%rbx); jne .LcatNum
    movl $30, %eax; jmp .LcatEmit

.TryP:
    cmpb $80, %al; jne .TryR
    cmpb $76, 1(%rbx); jne .LcatNum
    cmpb $85, 2(%rbx); jne .LcatNum
    cmpb $83, 3(%rbx); jne .LcatNum
    movl $10, %eax; jmp .LcatEmit

.TryR:
    cmpb $82, %al; jne .TryD
    cmpb $69, 1(%rbx); jne .LcatNum
    cmpb $67, 2(%rbx); jne .LcatNum
    cmpb $79, 3(%rbx); jne .LcatNum
    cmpb $82, 4(%rbx); jne .LcatNum
    cmpb $68, 5(%rbx); jne .LcatNum
    movl $28, %eax; jmp .LcatEmit

.TryD:
    cmpb $68, %al; jne .TryM
    cmpb $69, 1(%rbx); je .TryDEF
    cmpb $73, 1(%rbx); jne .TryDX
    movl $13, %eax; jmp .LcatEmit
.TryDEF:
    cmpb $70, 2(%rbx); jne .LcatNum
    movl $6, %eax; jmp .LcatEmit
.TryDX:
    cmpb $49, 1(%rbx); movl $1, %eax; je .LcatEmit
    cmpb $50, 1(%rbx); movl $2, %eax; je .LcatEmit
    cmpb $51, 1(%rbx); movl $3, %eax; je .LcatEmit
    cmpb $52, 1(%rbx); movl $4, %eax; je .LcatEmit
    cmpb $53, 1(%rbx); movl $5, %eax; je .LcatEmit
    cmpb $54, 1(%rbx); movl $6, %eax; je .LcatEmit
    cmpb $55, 1(%rbx); movl $7, %eax; je .LcatEmit
    cmpb $56, 1(%rbx); movl $8, %eax; je .LcatEmit
    cmpb $57, 1(%rbx); movl $9, %eax; je .LcatEmit
    cmpb $48, 1(%rbx); movl $0, %eax; je .LcatEmit
    jmp .LcatNum

.TryM:
    cmpb $77, %al; jne .TryEQ
    cmpb $73, 1(%rbx); jne .TryM2
    cmpb $78, 2(%rbx); jne .TryM2
    cmpb $85, 3(%rbx); jne .TryM2
    cmpb $83, 4(%rbx); jne .TryM2
    movl $11, %eax; jmp .LcatEmit
.TryM2:
    cmpb $85, 1(%rbx); jne .LcatNum
    cmpb $76, 2(%rbx); jne .LcatNum
    movl $12, %eax; jmp .LcatEmit

.TryEQ:
    cmpb $69, %al; jne .LcatNum
    cmpb $81, 1(%rbx); jne .LcatNum
    movl $14, %eax; jmp .LcatEmit

.LcatNum:
    movq %rbx, %rdi; callq _atoi
.LcatEmit:
    movb %al, Lbuf(%rip)
    leaq Lbuf(%rip), %rsi; movq %r14, %rdi; movl $1, %edx; callq _write
    incq %r15; jmp .LcatLoop
.LcatDone:
    movq %r14, %rdi; callq _close
    leaq Lcat(%rip), %rdi; xorl %eax, %eax; callq _printf
    jmp done

run_mode:
    movq 8(%r13), %rdi; xorl %esi, %esi; callq _open
    testq %rax, %rax; js err; movq %rax, %r14
    leaq Lbuf(%rip), %rsi; movq %r14, %rdi; movl $4096, %edx; callq _read
    movq %rax, %r15
    movq %r14, %rdi; callq _close
    leaq Lok(%rip), %rdi; movq %r15, %rsi; xorl %eax, %eax; callq _printf
    jmp done

usage:
    leaq Luse(%rip), %rdi; xorl %eax, %eax; callq _printf; jmp done
err:
    leaq Lerr(%rip), %rdi; xorl %eax, %eax; callq _printf
done:
    popq %rbx; popq %r15; popq %r14; popq %r13; popq %r12; popq %rbp; retq

.data
Luse: .asciz "5bit: ./5bit <file> | ./5bit cat <out.5b> START END PLUS D0-D9 DEF RECORD EMIT...\n"
Lerr: .asciz "Error\n"
Lok:  .asciz "OK: %lld bytes\n"
Lcat: .asciz "Created.\n"
.bss
Lbuf: .space 16384
