/*
 * Greenhorn FLUX VM — Fibonacci test
 * Computes fib(10) = 55 using the FLUX ISA
 * Build: gcc -std=gnu99 -Wall -O2 -Os -o test_fib test_fib.c vm.c
 */
#include "vm.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    GHVM vm;
    gh_vm_init(&vm);

    /* fib(10) = 55
     * r0=a, r1=b, r2=counter, r3=tmp
     * Bytecode layout:
     *   0-3:   MOVI r0, 0
     *   4-7:   MOVI r1, 1
     *   8-11:  MOVI r2, 10
     *  12-13:  DEC r2
     *  14-17:  JZ r2, +16 → done@34
     *  18-20:  MOV r3, r0
     *  21-23:  ADD r3, r1
     *  24-26:  MOV r0, r1
     *  27-29:  MOV r1, r3
     *  30-33:  JMP -22 → loop@12
     *  34-35:  PRNT r1
     *  36:     HALT
     */
    uint8_t prog[] = {
        0x2B, 0, 0, 0,       /* MOVI r0, 0 */
        0x2B, 1, 1, 0,       /* MOVI r1, 1 */
        0x2B, 2, 10, 0,      /* MOVI r2, 10 */
        0x0F, 2,              /* DEC r2 */
        0x05, 2, 16, 0,      /* JZ r2, +16 */
        0x01, 3, 0,           /* MOV r3, r0 */
        0x08, 3, 1,           /* ADD r3, r1 */
        0x01, 0, 1,           /* MOV r0, r1 */
        0x01, 1, 3,           /* MOV r1, r3 */
        0x04, 0, 0xEA, 0xFF,  /* JMP -22 */
        0x62, 1,              /* PRNT r1 */
        0x80                  /* HALT */
    };

    printf("Greenhorn FLUX VM — Fibonacci test\n");
    printf("Computing fib(10), expecting 55...\n");

    gh_vm_load(&vm, prog, sizeof(prog));
    int64_t cycles = gh_vm_execute(&vm);

    printf("Cycles: %ld\n", (long)cycles);
    printf("Error: %s\n", gh_error_str(vm.last_error));

    if (vm.regs[1] == 55) {
        printf("PASS ✓\n");
        return 0;
    }
    printf("FAIL — r1=%d\n", vm.regs[1]);
    return 1;
}
