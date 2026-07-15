.globl _main
_main:
    pushq %rbp; movq %rsp, %rbp; pushq %r12; pushq %r13; pushq %r14; pushq %r15
    movq %rdi, %r12; movq %rsi, %r13
    cmpq $2, %r12; jl usage
    movq 8(%r13), %rdi; cmpb $99, (%rdi); jne run_mode
    cmpq $4, %r12; jl usage
    movq 16(%r13), %rdi; movl $0x601, %esi; movl $0x1B6, %edx
    callq _open; testq %rax, %rax; js err; movq %rax, %r14
    movb $0, Lbuf(%rip)
    leaq Lbuf(%rip), %rsi; movq %r14, %rdi; movl $1, %edx; callq _write
    movl $3, %r15d
1:  cmpq %r12, %r15; jge 2f
    movq (%r13,%r15,8), %rdi; callq _atoi
    movb %al, Lbuf(%rip)
    leaq Lbuf(%rip), %rsi; movq %r14, %rdi; movl $1, %edx; callq _write
    incq %r15; jmp 1b
2:  movq %r14, %rdi; callq _close
    leaq Lcat(%rip), %rdi; xorl %eax, %eax; callq _printf
    jmp done

run_mode:
    movq 8(%r13), %rdi; xorl %esi, %esi; callq _open
    testq %rax, %rax; js err; movq %rax, %r14
    leaq Lbuf(%rip), %rsi; movq %r14, %rdi; movl $16384, %edx; callq _read
    movq %r14, %rdi; callq _close
    leaq Lbuf+1(%rip), %r12
    leaq Vbuf(%rip), %r13
    xorq %r14, %r14
    xorq %r15, %r15
3:  cmpq $4096, %r15; jge 9f
    movzbl (%r12,%r15), %eax; incq %r15
    cmpb $28, %al; je 9f
    cmpb $30, %al; je 3b
    cmpb $31, %al; je 3b
    cmpb $6, %al; je 3b
    cmpb $14, %al; je 8f
    cmpb $10, %al; je 7f
    cmpb $9, %al; jbe 4f
    jmp 3b
4:  movzbq %al, %rax
5:  cmpq $4096, %r15; jge 6f
    movzbl (%r12,%r15), %ecx
    cmpb $30, %cl; je 6f
    cmpb $9, %cl; ja 6f
    imulq $10, %rax; addq %rcx, %rax; incq %r15; jmp 5b
6:  incq %r15; movq %rax, (%r13); addq $8, %r13; jmp 3b
7:  subq $8, %r13; movq (%r13), %rax
    subq $8, %r13; movq (%r13), %rcx
    addq %rcx, %rax; movq %rax, (%r13); addq $8, %r13; jmp 3b
8:  subq $8, %r13; movq (%r13), %r14
9:  leaq Lres(%rip), %rdi; movq %r14, %rsi; xorl %eax, %eax; callq _printf
    jmp done

usage: leaq Luse(%rip), %rdi; xorl %eax, %eax; callq _printf; jmp done
err:   leaq Lerr(%rip), %rdi; xorl %eax, %eax; callq _printf
done:  popq %r15; popq %r14; popq %r13; popq %r12; popq %rbp; retq

.data
Luse: .asciz "5bit <file.5b> | 5bit cat <out.5b> <tokens...>\n"
Lerr: .asciz "Error\n"
Lcat: .asciz "Created.\n"
Lres: .asciz "[%lld]\n"
.bss
Lbuf: .space 16384
Vbuf: .space 16384
