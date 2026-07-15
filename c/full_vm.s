.globl _main
_main:
    pushq %rbp; movq %rsp, %rbp; pushq %r12; pushq %r13; pushq %r14; pushq %r15
    movq %rdi, %r12; movq %rsi, %r13
    cmpq $2, %r12; jl usage
    movq 8(%r13), %rdi; xorl %esi, %esi; callq _open
    testq %rax, %rax; js err; movq %rax, %r14
    leaq Lbuf(%rip), %rsi; movq %r14, %rdi; movl $4096, %edx; callq _read
    movq %rax, %r15; movq %r14, %rdi; callq _close
    leaq Lbuf+1(%rip), %r12
    leaq Vbuf(%rip), %r13
    xorq %r14, %r14
    xorq %r15, %r15
3:  cmpq $4096, %r15; jge 9f
    movzbl (%r12,%r15), %eax; incq %r15
    cmpb $28, %al; je 9f
    cmpb $30, %al; je 3b
    cmpb $31, %al; je 3b
    cmpb $14, %al; je 8f
    cmpb $10, %al; je 7f
    cmpb $11, %al; je 100f
    cmpb $12, %al; je 101f
    cmpb $13, %al; je 102f
    cmpb $15, %al; je 103f
    cmpb $16, %al; je 104f
    cmpb $17, %al; je 105f
    cmpb $18, %al; je 106f
    cmpb $21, %al; je 107f
    cmpb $9, %al; jbe 4f
    jmp 3b
4:  movzbq %al, %rax
5:  cmpq $4096, %r15; jge 6f
    movzbl (%r12,%r15), %ecx
    cmpb $30, %cl; je 6f; cmpb $9, %cl; ja 6f
    imulq $10, %rax; addq %rcx, %rax; incq %r15; jmp 5b
6:  incq %r15; movq %rax, (%r13); addq $8, %r13; jmp 3b
7:  subq $8, %r13; movq (%r13), %rax
    subq $8, %r13; addq %rax, (%r13); addq $8, %r13; jmp 3b
100: subq $8, %r13; movq (%r13), %rax
    subq $8, %r13; subq %rax, (%r13); addq $8, %r13; jmp 3b
101: subq $8, %r13; movq (%r13), %rax
    subq $8, %r13; movq (%r13), %rdx
    imulq %rax, %rdx; movq %rdx, (%r13); addq $8, %r13; jmp 3b
102: subq $8, %r13; movq (%r13), %rcx
    subq $8, %r13; movq (%r13), %rax; cqto; idivq %rcx; movq %rax, (%r13); addq $8, %r13; jmp 3b
103: subq $8, %r13; movq (%r13), %rax
    subq $8, %r13; movq (%r13), %rdi
    subq $8, %r13; movq (%r13), %rsi
    subq $8, %r13; movq (%r13), %rdx; syscall
    movq %rax, (%r13); addq $8, %r13; jmp 3b
104: subq $8, %r13; movq (%r13), %rax
    subq $8, %r13; andq %rax, (%r13); addq $8, %r13; jmp 3b
105: subq $8, %r13; movq (%r13), %rax
    subq $8, %r13; orq %rax, (%r13); addq $8, %r13; jmp 3b
106: subq $8, %r13; movq (%r13), %rax
    subq $8, %r13; xorq %rax, (%r13); addq $8, %r13; jmp 3b
107: subq $8, %r13; notq (%r13); addq $8, %r13; jmp 3b
8:  subq $8, %r13; movq (%r13), %r14
9:  leaq Lres(%rip), %rdi; movq %r14, %rsi; xorl %eax, %eax; callq _printf
    jmp done
usage: leaq Luse(%rip), %rdi; xorl %eax, %eax; callq _printf; jmp done
err:   leaq Lerr(%rip), %rdi; xorl %eax, %eax; callq _printf
done:  popq %r15; popq %r14; popq %r13; popq %r12; popq %rbp; retq
.data
Luse: .asciz "5bit <file.5b>\n"
Lerr: .asciz "Error\n"
Lres: .asciz "[%lld]\n"
.bss
Lbuf: .space 16384
Vbuf: .space 16384
