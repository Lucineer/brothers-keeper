/*
 * Greenhorn FLUX VM — minimal portable C VM (zero deps, no malloc)
 * 64-register file, 64KB bytecode, 50 essential opcodes
 */
#ifndef GREENHORN_VM_H
#define GREENHORN_VM_H

#include <stdint.h>

/* Register layout: 32 GP (0-31), 16 FP (32-47), 16 special (48-63) */
#define GP_REGS   32
#define FP_REGS   16
#define SP_REGS   16
#define TOTAL_REGS 64
#define MEM_SIZE  (64 * 1024)  /* 64KB bytecode */
#define DATA_SIZE (64 * 1024)  /* 64KB data memory */
#define STACK_SIZE 4096
#define MAX_FRAMES 256
#define MAX_BOXES  64

/* Opcodes — compatible with FLUX ISA v2 */
typedef enum {
    OP_NOP=0x00, OP_MOV=0x01, OP_LOAD=0x02, OP_STORE=0x03,
    OP_JMP=0x04, OP_JZ=0x05, OP_JNZ=0x06, OP_CALL=0x07,
    OP_ADD=0x08, OP_SUB=0x09, OP_MUL=0x0A, OP_DIV=0x0B,
    OP_MOD=0x0C, OP_NEG=0x0D, OP_INC=0x0E, OP_DEC=0x0F,
    OP_AND=0x10, OP_OR=0x11, OP_XOR=0x12, OP_NOT=0x13,
    OP_SHL=0x14, OP_SHR=0x15, OP_ROTL=0x16, OP_ROTR=0x17,
    OP_CMP=0x18, OP_EQ=0x19, OP_NE=0x1A, OP_LT=0x1B, OP_LE=0x1C,
    OP_GT=0x1D, OP_GE=0x1E, OP_TEST=0x1F, OP_SETCC=0x20,
    OP_PUSH=0x21, OP_POP=0x22, OP_DUP=0x23, OP_SWAP=0x24,
    OP_ENTER=0x25, OP_LEAVE=0x26, OP_RET=0x28, OP_MOVI=0x2B,
    OP_JE=0x2E, OP_JNE=0x2F, OP_JL=0x36, OP_JGE=0x37,
    OP_CAST=0x38, OP_BOX=0x39, OP_UNBOX=0x3A,
    OP_FADD=0x40, OP_FSUB=0x41, OP_FMUL=0x42, OP_FDIV=0x43,
    OP_FNEG=0x44, OP_FABS=0x45, OP_FEQ=0x48, OP_FLT=0x49,
    OP_PUT=0x60, OP_GET=0x61, OP_PRNT=0x62,
    OP_SPAWN=0x63, OP_WAIT=0x64, OP_SEND=0x66, OP_RECV=0x67,
    OP_GAUGE=0x68, OP_ENERGY=0x69, OP_TRUST=0x70,
    OP_HALT=0x80, OP_YIELD=0x81,
} GreenhornOp;

/* Errors */
typedef enum {
    GH_OK=0, GH_ERR_HALT=1, GH_ERR_OPCODE=2, GH_ERR_DIVZERO=3,
    GH_ERR_STACK=4, GH_ERR_BUDGET=11
} GHError;

typedef struct {
    int type_tag;
    int32_t ival;
    float   fval;
} GHBox;

typedef struct {
    int32_t regs[TOTAL_REGS];
    uint8_t bytecode[MEM_SIZE];
    uint8_t data[DATA_SIZE];
    int32_t stack[STACK_SIZE];
    uint32_t frame_stack[MAX_FRAMES];
    uint32_t frame_count;
    GHBox boxes[MAX_BOXES];
    int box_count;
    uint8_t flag_zero, flag_sign;
    uint32_t pc, sp, cycle_count, max_cycles;
    uint8_t running, halted;
    GHError last_error;
} GHVM;

void gh_vm_init(GHVM *v);
void gh_vm_load(GHVM *v, const uint8_t *bc, uint32_t len);
int64_t gh_vm_execute(GHVM *v);
int64_t gh_vm_step(GHVM *v);
const char *gh_error_str(GHError e);

#endif
