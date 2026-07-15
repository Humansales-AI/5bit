.globl _main
.text
_main:
    pushq %rbp; movq %rsp, %rbp
    pushq %r12; pushq %r13; pushq %r14; pushq %r15
    movq %rdi, %r12; movq %rsi, %r13
    cmpq $2, %r12; jl usage

    movq 8(%r13), %rdi
    cmpb $99, (%rdi)
    jne run_mode

    cmpq $4, %r12; jl usage
    movq 16(%r13), %rdi; movl $0x601, %esi; movl $0x1B6, %edx; callq _open
    testq %rax, %rax; js err; movq %rax, %r14
    movb $0, Lbuf(%rip)
    leaq Lbuf(%rip), %rsi; movq %r14, %rdi; movl $1, %edx; callq _write
    movl $3, %r15d
.LcatLoop:
    cmpq %r12, %r15; jge .LcatDone
    movq (%r13,%r15,8), %rdi; callq _atoi
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
    popq %r15; popq %r14; popq %r13; popq %r12; popq %rbp; retq

.data
Luse: .asciz "5bit: ./5bit <file.5b> | ./5bit cat <out.5b> <tok1>...\n"
Lerr: .asciz "Error\n"
Lok:  .asciz "OK: %lld bytes\n"
Lcat: .asciz "Created.\n"
.bss
Lbuf: .space 16384
