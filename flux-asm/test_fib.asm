; test_fib.asm — Fibonacci: compute fib(10) = 55
; Uses iterative approach, stores result in R0
; After HALT, R0 should contain 55

.org 0x0000

    MOVI R0, 0         ; a = 0
    MOVI R1, 1         ; b = 1
    MOVI R2, 10        ; counter = 10

.loop
    MOV  R3, R1        ; temp = b
    ADD  R1, R0        ; b = a + b
    MOV  R0, R3        ; a = temp
    DEC  R2            ; counter--
    JNZ  .loop         ; if counter != 0, loop

    ; R0 = fib(10) = 55
    PRNT R0            ; print result
    HALT
