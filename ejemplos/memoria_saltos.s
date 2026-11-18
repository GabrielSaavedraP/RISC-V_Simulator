.global _start
_start:
    li s0, 0x1000
    li t0, 0x123480ff
    sw t0, 0(s0)
    lb s1, 0(s0)
    lbu s2, 0(s0)
    lh s3, 0(s0)
    lhu s4, 0(s0)
    srai s5, s3, 8
    srli s6, s3, 8
    li t0, 0x1234
    sh t0, 4(s0)
    li t0, 0x80
    sb t0, 6(s0)
    li t0, -1
    bne s1, t0, error
    li t0, 255
    bne s2, t0, error
    li t0, -32513
    bne s3, t0, error
    li t0, 33023
    bne s4, t0, error
    li t0, -128
    bne s5, t0, error
    li t0, 0x00ffff80
    bne s6, t0, error
    bge s3, zero, error
    bltu s3, s4, error
    la t5, duplicar
    li a1, 21
    jalr ra, 0(t5)
    li t0, 42
    bne a1, t0, error
    li a0, 1
    ebreak
error:
    li a0, 0
    ebreak
duplicar:
    add a1, a1, a1
    ret
