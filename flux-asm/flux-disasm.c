/*
 * flux-disasm.c — Disassembler for Greenhorn FLUX VM bytecode
 * Usage: flux-disasm input.bin [-o output.asm]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

enum {
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
};

static const char *opnames[256];

static void init_names(void) {
    memset(opnames, 0, sizeof(opnames));
    opnames[OP_NOP]="NOP"; opnames[OP_MOV]="MOV"; opnames[OP_LOAD]="LOAD";
    opnames[OP_STORE]="STORE"; opnames[OP_JMP]="JMP"; opnames[OP_JZ]="JZ";
    opnames[OP_JNZ]="JNZ"; opnames[OP_CALL]="CALL";
    opnames[OP_ADD]="ADD"; opnames[OP_SUB]="SUB"; opnames[OP_MUL]="MUL";
    opnames[OP_DIV]="DIV"; opnames[OP_MOD]="MOD"; opnames[OP_NEG]="NEG";
    opnames[OP_INC]="INC"; opnames[OP_DEC]="DEC";
    opnames[OP_AND]="AND"; opnames[OP_OR]="OR"; opnames[OP_XOR]="XOR";
    opnames[OP_NOT]="NOT"; opnames[OP_SHL]="SHL"; opnames[OP_SHR]="SHR";
    opnames[OP_ROTL]="ROTL"; opnames[OP_ROTR]="ROTR";
    opnames[OP_CMP]="CMP"; opnames[OP_EQ]="EQ"; opnames[OP_NE]="NE";
    opnames[OP_LT]="LT"; opnames[OP_LE]="LE"; opnames[OP_GT]="GT";
    opnames[OP_GE]="GE"; opnames[OP_TEST]="TEST"; opnames[OP_SETCC]="SETCC";
    opnames[OP_PUSH]="PUSH"; opnames[OP_POP]="POP"; opnames[OP_DUP]="DUP";
    opnames[OP_SWAP]="SWAP"; opnames[OP_ENTER]="ENTER"; opnames[OP_LEAVE]="LEAVE";
    opnames[OP_RET]="RET"; opnames[OP_MOVI]="MOVI";
    opnames[OP_JE]="JE"; opnames[OP_JNE]="JNE"; opnames[OP_JL]="JL";
    opnames[OP_JGE]="JGE";
    opnames[OP_CAST]="CAST"; opnames[OP_BOX]="BOX"; opnames[OP_UNBOX]="UNBOX";
    opnames[OP_FADD]="FADD"; opnames[OP_FSUB]="FSUB"; opnames[OP_FMUL]="FMUL";
    opnames[OP_FDIV]="FDIV"; opnames[OP_FNEG]="FNEG"; opnames[OP_FABS]="FABS";
    opnames[OP_FEQ]="FEQ"; opnames[OP_FLT]="FLT";
    opnames[OP_PUT]="PUT"; opnames[OP_GET]="GET"; opnames[OP_PRNT]="PRNT";
    opnames[OP_SPAWN]="SPAWN"; opnames[OP_WAIT]="WAIT";
    opnames[OP_SEND]="SEND"; opnames[OP_RECV]="RECV";
    opnames[OP_GAUGE]="GAUGE"; opnames[OP_ENERGY]="ENERGY";
    opnames[OP_TRUST]="TRUST";
    opnames[OP_HALT]="HALT"; opnames[OP_YIELD]="YIELD";
}

static char regname_buf[2][8];
static int regname_idx = 0;

static const char *regname(uint8_t r) {
    char *buf = regname_buf[regname_idx++ & 1];
    if (r < 32) { snprintf(buf, 8, "R%d", r); return buf; }
    if (r < 48) { snprintf(buf, 8, "FP%d", r - 32); return buf; }
    snprintf(buf, 8, "S%d", r - 48);
    return buf;
}

/* Instruction format categories */
enum { FMT_NONE, FMT_RD, FMT_RD_RS, FMT_RD_IMM16, FMT_JMP_IMM16, FMT_FLUX_SKIP };

static int fmt_of(uint8_t op) {
    switch (op) {
    case OP_NOP: case OP_DUP: case OP_SWAP: case OP_FABS:
    case OP_RET: case OP_HALT: case OP_YIELD: return FMT_NONE;
    case OP_NEG: case OP_INC: case OP_DEC: case OP_FNEG:
    case OP_PUSH: case OP_POP: case OP_PUT: case OP_GET: case OP_PRNT:
    case OP_ENTER: case OP_LEAVE: return FMT_RD;
    case OP_MOV: case OP_LOAD: case OP_STORE:
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_AND: case OP_OR: case OP_XOR: case OP_NOT:
    case OP_SHL: case OP_SHR: case OP_ROTL: case OP_ROTR:
    case OP_CMP: case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
    case OP_GT: case OP_GE: case OP_TEST: case OP_SETCC:
    case OP_CAST: case OP_BOX: case OP_UNBOX:
    case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV:
    case OP_FEQ: case OP_FLT: return FMT_RD_RS;
    case OP_MOVI: return FMT_RD_IMM16;
    case OP_JMP: case OP_JZ: case OP_JNZ: case OP_JE: case OP_JNE:
    case OP_JL: case OP_JGE: case OP_CALL: return FMT_JMP_IMM16;
    case OP_SPAWN: case OP_WAIT: case OP_SEND: case OP_RECV:
    case OP_GAUGE: case OP_ENERGY: case OP_TRUST: return FMT_FLUX_SKIP;
    default: return FMT_NONE;
    }
}

static void disasm(const uint8_t *bc, uint32_t len, FILE *out) {
    uint32_t pc = 0;
    while (pc < len) {
        uint32_t addr = pc;
        uint8_t op = bc[pc++];
        int fmt = fmt_of(op);
        const char *name = opnames[op] ? opnames[op] : "???";

        fprintf(out, "  %04x: ", addr);

        switch (fmt) {
        case FMT_NONE:
            fprintf(out, "%s\n", name);
            break;
        case FMT_RD: {
            uint8_t rd = (pc < len) ? bc[pc++] : 0;
            fprintf(out, "%s %s\n", name, regname(rd));
            break;
        }
        case FMT_RD_RS: {
            uint8_t rd = (pc < len) ? bc[pc++] : 0;
            uint8_t rs = (pc < len) ? bc[pc++] : 0;
            if (op == OP_LOAD)
                fprintf(out, "%s %s, [%s]\n", name, regname(rd), regname(rs));
            else if (op == OP_STORE)
                fprintf(out, "%s %s, [%s]\n", name, regname(rs), regname(rd));
            else
                fprintf(out, "%s %s, %s\n", name, regname(rd), regname(rs));
            break;
        }
        case FMT_RD_IMM16: {
            uint8_t rd = (pc < len) ? bc[pc++] : 0;
            int16_t imm = 0;
            if (pc + 1 < len) {
                imm = (int16_t)(bc[pc] | (bc[pc+1] << 8));
                pc += 2;
            }
            fprintf(out, "%s %s, %d\n", name, regname(rd), imm);
            break;
        }
        case FMT_JMP_IMM16: {
            uint8_t rd = (pc < len) ? bc[pc++] : 0;
            int16_t imm = 0;
            if (pc + 1 < len) {
                imm = (int16_t)(bc[pc] | (bc[pc+1] << 8));
                pc += 2;
            }
            uint32_t target = (uint32_t)((int32_t)pc + imm);
            /* Detect backward jumps (loop backedges) */
            if (target <= addr)
                fprintf(out, "%s .L_%04x    ; loop backedge\n", name, target);
            else
                fprintf(out, "%s .L_%04x\n", name, target);
            (void)rd;
            break;
        }
        case FMT_FLUX_SKIP: {
            uint16_t skip = 0;
            if (pc + 1 < len) {
                skip = (uint16_t)(bc[pc] | (bc[pc+1] << 8));
                pc += 2;
            }
            /* Show the skip payload as bytes */
            if (skip > 0 && pc + skip <= len) {
                fprintf(out, "%s 0, ", name);
                for (uint16_t i = 0; i < skip && i < 16; i++)
                    fprintf(out, "%02x ", bc[pc + i]);
                if (skip > 16) fprintf(out, "...");
                fprintf(out, "\n");
                pc += skip;
            } else {
                fprintf(out, "%s 0\n", name);
            }
            break;
        }
        default:
            fprintf(out, "DB 0x%02x\n", op);
            break;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: flux-disasm input.bin [-o output.asm]\n");
        return 1;
    }

    init_names();

    const char *infile = argv[1];
    const char *outfile = NULL;
    FILE *out = stdout;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outfile = argv[++i];
        }
    }

    if (outfile) {
        out = fopen(outfile, "w");
        if (!out) { perror(outfile); return 1; }
    }

    FILE *f = fopen(infile, "rb");
    if (!f) { perror(infile); return 1; }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *bc = malloc(len);
    if (!bc) { perror("malloc"); return 1; }
    if ((long)fread(bc, 1, len, f) != len) { perror("fread"); return 1; }
    fclose(f);

    fprintf(out, "; flux-disasm: %s (%ld bytes)\n", infile, len);
    disasm(bc, (uint32_t)len, out);

    free(bc);
    if (outfile) fclose(out);
    return 0;
}
