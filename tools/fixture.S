# HOI4-shaped SDL dynapi fixture: real Win64 ABI, pdata and bootstrap installer.
.text
.p2align 4
.globl FixtureLoader
.seh_proc FixtureLoader
FixtureLoader:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    movq 80(%rsp), %rax
    movq %rax, 32(%rsp)
    call FixtureStub
    testq %rax, %rax
    leaq FixtureAnchor(%rip), %r10
    .space 260, 0x90
    addq $40, %rsp
    ret
.seh_endproc

.p2align 4
.globl FixtureStub
FixtureStub:
    rex.W jmp *FixtureSlot(%rip)
    .space 9, 0xcc

.p2align 4
.seh_proc FixtureBootstrap
FixtureBootstrap:
    subq $72, %rsp
    .seh_stackalloc 72
    .seh_endprologue
    movq %rcx, 32(%rsp)
    movq %rdx, 40(%rsp)
    movq %r8, 48(%rsp)
    movq %r9, 56(%rsp)
    call FixtureInstall
    movq 32(%rsp), %rcx
    movq 40(%rsp), %rdx
    movq 48(%rsp), %r8
    movq 56(%rsp), %r9
    addq $72, %rsp
    rex.W jmp *FixtureSlot(%rip)
.seh_endproc

.p2align 4
FixtureInstall:
    leaq FixtureWave(%rip), %rcx
    movq %rcx, FixtureSlot(%rip)
    ret

.data
.p2align 3
.globl FixtureSlot
FixtureSlot:
    .quad FixtureBootstrap

.section .rdata,"dr"
FixtureAnchor:
    .asciz "Failed to load audio \"%s\". SDL Error: \"%s\"\n"
