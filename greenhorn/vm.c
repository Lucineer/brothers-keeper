/*
 * Greenhorn FLUX VM — minimal portable C implementation
 * No malloc, no libm, no external deps. Compiles anywhere.
 */
#include "vm.h"
#include <string.h>
#include <stdio.h>

/* Fetch helpers */
static inline uint8_t f8(GHVM *v) { return v->bytecode[v->pc++]; }
static inline int16_t fi16(GHVM *v) {
    uint8_t l = v->bytecode[v->pc++], h = v->bytecode[v->pc++];
    return (int16_t)(l | (h << 8));
}

/* Flags */
static inline void sf(GHVM *v, int32_t r) { v->flag_zero = (r == 0); v->flag_sign = (r < 0); }
static inline void scf(GHVM *v, int32_t a, int32_t b) { v->flag_zero = (a == b); v->flag_sign = (a < b); }

/* Stack */
static inline int spush(GHVM *v, int32_t val) {
    if (v->sp >= STACK_SIZE) return 1;
    v->stack[v->sp++] = val; return 0;
}
static inline int32_t spop(GHVM *v) {
    if (v->sp <= 0) return 0;
    return v->stack[--v->sp];
}

#define ERR(e) do { v->last_error=(e); v->running=0; return -(int64_t)(e); } while(0)
#define GPR (v->regs)

void gh_vm_init(GHVM *v) {
    memset(v, 0, sizeof(*v));
    v->sp = 0;
    v->max_cycles = 10000000;
}

void gh_vm_load(GHVM *v, const uint8_t *bc, uint32_t len) {
    uint32_t copy = len < MEM_SIZE ? len : MEM_SIZE;
    memset(v, 0, sizeof(*v));
    v->max_cycles = 10000000;
    if (bc && copy > 0) memcpy(v->bytecode, bc, copy);
    v->sp = 0;
}

int64_t gh_vm_execute(GHVM *v) {
    v->running = 1; v->halted = 0;
    uint8_t op, rd, rs1;
    int16_t imm;

    while (v->running && v->cycle_count < v->max_cycles) {
        op = f8(v);
        v->cycle_count++;

        switch (op) {

        /* NOP */
        case OP_NOP: break;

        /* MOV rd, rs1 */
        case OP_MOV: rd=f8(v); rs1=f8(v); GPR[rd]=GPR[rs1]; break;

        /* MOVI rd, imm16 */
        case OP_MOVI: rd=f8(v); imm=fi16(v); GPR[rd]=imm; break;

        /* LOAD rd, [rs1] — load from data memory */
        case OP_LOAD: rd=f8(v); rs1=f8(v); {
            uint32_t addr = (uint32_t)GPR[rs1];
            if (addr + 3 < DATA_SIZE) {
                GPR[rd] = (int32_t)((uint32_t)v->data[addr]
                    | ((uint32_t)v->data[addr+1]<<8)
                    | ((uint32_t)v->data[addr+2]<<16)
                    | ((uint32_t)v->data[addr+3]<<24));
            }
        } break;

        /* STORE [rd], rs1 */
        case OP_STORE: rd=f8(v); rs1=f8(v); {
            uint32_t addr = (uint32_t)GPR[rd];
            int32_t val = GPR[rs1];
            if (addr + 3 < DATA_SIZE) {
                v->data[addr]   = (uint8_t)(val);
                v->data[addr+1] = (uint8_t)(val>>8);
                v->data[addr+2] = (uint8_t)(val>>16);
                v->data[addr+3] = (uint8_t)(val>>24);
            }
        } break;

        /* Arithmetic */
        case OP_ADD: rd=f8(v); rs1=f8(v); { int32_t r=GPR[rd]+GPR[rs1]; sf(v,r); GPR[rd]=r; } break;
        case OP_SUB: rd=f8(v); rs1=f8(v); { int32_t r=GPR[rd]-GPR[rs1]; sf(v,r); GPR[rd]=r; } break;
        case OP_MUL: rd=f8(v); rs1=f8(v); { int32_t r=GPR[rd]*GPR[rs1]; sf(v,r); GPR[rd]=r; } break;
        case OP_DIV: rd=f8(v); rs1=f8(v); if(!GPR[rs1]) ERR(GH_ERR_DIVZERO); GPR[rd]=GPR[rd]/GPR[rs1]; break;
        case OP_MOD: rd=f8(v); rs1=f8(v); if(!GPR[rs1]) ERR(GH_ERR_DIVZERO); GPR[rd]=GPR[rd]%GPR[rs1]; break;
        case OP_NEG: rd=f8(v); { int32_t r=-GPR[rd]; sf(v,r); GPR[rd]=r; } break;
        case OP_INC: rd=f8(v); { int32_t r=GPR[rd]+1; sf(v,r); GPR[rd]=r; } break;
        case OP_DEC: rd=f8(v); { int32_t r=GPR[rd]-1; sf(v,r); GPR[rd]=r; } break;

        /* Bitwise */
        case OP_AND: rd=f8(v); rs1=f8(v); GPR[rd]&=GPR[rs1]; break;
        case OP_OR:  rd=f8(v); rs1=f8(v); GPR[rd]|=GPR[rs1]; break;
        case OP_XOR: rd=f8(v); rs1=f8(v); GPR[rd]^=GPR[rs1]; break;
        case OP_NOT: rd=f8(v); GPR[rd]=~GPR[rd]; break;
        case OP_SHL: rd=f8(v); rs1=f8(v); GPR[rd]<<=GPR[rs1]; break;
        case OP_SHR: rd=f8(v); rs1=f8(v); GPR[rd]>>=GPR[rs1]; break;
        case OP_ROTL: rd=f8(v); rs1=f8(v); { uint32_t w=(uint32_t)GPR[rd]; int s=GPR[rs1]&31; GPR[rd]=(int32_t)((w<<s)|(w>>(32-s))); } break;
        case OP_ROTR: rd=f8(v); rs1=f8(v); { uint32_t w=(uint32_t)GPR[rd]; int s=GPR[rs1]&31; GPR[rd]=(int32_t)((w>>s)|(w<<(32-s))); } break;

        /* Comparison */
        case OP_CMP: rd=f8(v); rs1=f8(v); scf(v,GPR[rd],GPR[rs1]); break;
        case OP_EQ:  rd=f8(v); rs1=f8(v); GPR[rd]=(GPR[rd]==GPR[rs1]); break;
        case OP_NE:  rd=f8(v); rs1=f8(v); GPR[rd]=(GPR[rd]!=GPR[rs1]); break;
        case OP_LT:  rd=f8(v); rs1=f8(v); GPR[rd]=(GPR[rd]<GPR[rs1]); break;
        case OP_LE:  rd=f8(v); rs1=f8(v); GPR[rd]=(GPR[rd]<=GPR[rs1]); break;
        case OP_GT:  rd=f8(v); rs1=f8(v); GPR[rd]=(GPR[rd]>GPR[rs1]); break;
        case OP_GE:  rd=f8(v); rs1=f8(v); GPR[rd]=(GPR[rd]>=GPR[rs1]); break;
        case OP_TEST: rd=f8(v); rs1=f8(v); sf(v, GPR[rd] & GPR[rs1]); break;
        case OP_SETCC: rd=f8(v); rs1=f8(v); GPR[rd]=(rs1==0)?v->flag_zero:(rs1==1)?v->flag_sign:0; break;

        /* Stack */
        case OP_PUSH: rd=f8(v); if(spush(v, GPR[rd])) ERR(GH_ERR_STACK); break;
        case OP_POP:  rd=f8(v); GPR[rd]=spop(v); break;
        case OP_DUP:  { int32_t val=spop(v); if(spush(v,val)||spush(v,val)) ERR(GH_ERR_STACK); } break;
        case OP_SWAP: { int32_t a=spop(v), b=spop(v); if(spush(v,a)||spush(v,b)) ERR(GH_ERR_STACK); } break;
        case OP_ENTER: rd=f8(v); if(v->frame_count>=MAX_FRAMES) ERR(GH_ERR_STACK); v->frame_stack[v->frame_count++]=v->sp; v->sp+=rd; break;
        case OP_LEAVE: rd=f8(v); if(!v->frame_count) ERR(GH_ERR_STACK); v->sp=v->frame_stack[--v->frame_count]+rd; break;

        /* Control flow */
        case OP_JMP: rd=f8(v); imm=fi16(v); v->pc+=imm; break;
        case OP_JZ:  rd=f8(v); imm=fi16(v); if(GPR[rd]==0) v->pc+=imm; break;
        case OP_JNZ: rd=f8(v); imm=fi16(v); if(GPR[rd]!=0) v->pc+=imm; break;
        case OP_JE:  rd=f8(v); imm=fi16(v); if(v->flag_zero) v->pc+=imm; break;
        case OP_JNE: rd=f8(v); imm=fi16(v); if(!v->flag_zero) v->pc+=imm; break;
        case OP_JL:  rd=f8(v); imm=fi16(v); if(v->flag_sign) v->pc+=imm; break;
        case OP_JGE: rd=f8(v); imm=fi16(v); if(!v->flag_sign) v->pc+=imm; break;
        case OP_CALL: rd=f8(v); imm=fi16(v); if(v->frame_count>=MAX_FRAMES) ERR(GH_ERR_STACK); v->frame_stack[v->frame_count++]=v->pc; v->pc+=imm; break;
        case OP_RET:  rd=f8(v); if(!v->frame_count) ERR(GH_ERR_STACK); v->pc=v->frame_stack[--v->frame_count]; break;

        /* Float */
        case OP_CAST: rd=f8(v); rs1=f8(v); {
            float *fp = (float*)&GPR[32];
            if(rs1==0) fp[rd&0xF]=(float)GPR[rd];
            else GPR[rd&0x1F]=(int32_t)fp[rs1&0xF];
        } break;
        case OP_FADD: rd=f8(v); rs1=f8(v); { float *fp=(float*)&GPR[32]; fp[rd&0xF]+=fp[rs1&0xF]; } break;
        case OP_FSUB: rd=f8(v); rs1=f8(v); { float *fp=(float*)&GPR[32]; fp[rd&0xF]-=fp[rs1&0xF]; } break;
        case OP_FMUL: rd=f8(v); rs1=f8(v); { float *fp=(float*)&GPR[32]; fp[rd&0xF]*=fp[rs1&0xF]; } break;
        case OP_FDIV: rd=f8(v); rs1=f8(v); { float *fp=(float*)&GPR[32]; if(fp[rs1&0xF]) fp[rd&0xF]/=fp[rs1&0xF]; } break;
        case OP_FNEG: rd=f8(v); { float *fp=(float*)&GPR[32]; fp[rd&0xF]=-fp[rd&0xF]; } break;
        case OP_FABS: { rd=f8(v); float *fp=(float*)&GPR[32]; float f=fp[rd&0xF]; fp[rd&0xF]=f<0?-f:f; } break;
        case OP_FEQ: rd=f8(v); rs1=f8(v); { float *fp=(float*)&GPR[32]; GPR[rd&0x1F]=(fp[rd&0xF]==fp[rs1&0xF]); } break;
        case OP_FLT: rd=f8(v); rs1=f8(v); { float *fp=(float*)&GPR[32]; GPR[rd&0x1F]=(fp[rd&0xF]<fp[rs1&0xF]); } break;

        /* I/O */
        case OP_PUT:  rd=f8(v); putchar(GPR[rd] & 0xFF); break;
        case OP_GET:  rd=f8(v); GPR[rd]=(int32_t)getchar(); break;
        case OP_PRNT: rd=f8(v); printf("%d\n", GPR[rd]); break;

        /* Box/Unbox */
        case OP_BOX: { f8(v); f8(v); if(v->box_count>=MAX_BOXES) ERR(GH_ERR_STACK);
            GHBox *b=&v->boxes[v->box_count++]; b->type_tag=f8(v);
            memcpy(&b->ival, &v->bytecode[v->pc], 4); v->pc+=4;
            GPR[0]=v->box_count-1;
        } break;
        case OP_UNBOX: { f8(v); f8(v); int id=f8(v);
            if(id<0||id>=v->box_count) ERR(GH_ERR_OPCODE);
            GHBox *b=&v->boxes[id];
            if(b->type_tag==0) GPR[0]=b->ival; else GPR[0]=b->ival?1:0;
        } break;

        /* FLUX agent opcodes — stubs that store results in r0 */
        case OP_SPAWN: { uint16_t l=f8(v)|((uint16_t)f8(v)<<8); v->pc+=l; GPR[0]=42; } break;
        case OP_WAIT:  { uint16_t l=f8(v)|((uint16_t)f8(v)<<8); v->pc+=l; GPR[0]=0; } break;
        case OP_SEND:  { uint16_t l=f8(v)|((uint16_t)f8(v)<<8); v->pc+=l; GPR[0]=1; } break;
        case OP_RECV:  { uint16_t l=f8(v)|((uint16_t)f8(v)<<8); v->pc+=l; GPR[0]=0; } break;
        case OP_GAUGE: { uint16_t l=f8(v)|((uint16_t)f8(v)<<8); v->pc+=l; GPR[0]=100; } break;
        case OP_ENERGY:{ uint16_t l=f8(v)|((uint16_t)f8(v)<<8); v->pc+=l; GPR[0]=999; } break;
        case OP_TRUST: { uint16_t l=f8(v)|((uint16_t)f8(v)<<8); v->pc+=l; GPR[0]=50; } break;

        case OP_YIELD: break;

        /* HALT */
        case OP_HALT:
            v->halted = 1; v->running = 0;
            v->last_error = GH_ERR_HALT;
            return (int64_t)v->cycle_count;

        default: ERR(GH_ERR_OPCODE);
        }
    }

    v->running = 0;
    if (v->cycle_count >= v->max_cycles && !v->halted) v->last_error = GH_ERR_BUDGET;
    return (int64_t)v->cycle_count;
}

int64_t gh_vm_step(GHVM *v) {
    if (!v->running) v->running = 1;
    if (v->cycle_count >= v->max_cycles) return GH_ERR_BUDGET;
    uint32_t saved = v->max_cycles;
    v->max_cycles = v->cycle_count + 1;
    int64_t rc = gh_vm_execute(v);
    v->max_cycles = saved;
    return rc < 0 ? (int)(-rc) : 0;
}

const char *gh_error_str(GHError e) {
    switch (e) {
        case GH_OK:       return "OK";
        case GH_ERR_HALT: return "HALT";
        case GH_ERR_OPCODE: return "INVALID_OPCODE";
        case GH_ERR_DIVZERO: return "DIV_ZERO";
        case GH_ERR_STACK: return "STACK_ERROR";
        case GH_ERR_BUDGET: return "CYCLE_BUDGET";
        default: return "UNKNOWN";
    }
}
