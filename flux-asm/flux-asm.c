/*
 * flux-asm.c — Two-pass assembler for Greenhorn FLUX VM bytecode
 * Usage: flux-asm input.asm [-o output.bin] [-s output.sym]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* Opcodes — must match vm.h */
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

#define MAX_LINE 1024
#define MAX_LABELS 4096
#define MAX_CODE  (64*1024)
#define MAX_FIXUPS 8192

static uint8_t code[MAX_CODE];
static uint32_t code_len;

typedef struct {
    char name[64];
    uint32_t addr;
} Label;

typedef struct {
    uint32_t patch_addr; /* where the imm16 lives */
    char label[64];
    int line;
} Fixup;

static Label labels[MAX_LABELS];
static int label_count;
static Fixup fixups[MAX_FIXUPS];
static int fixup_count;
static uint32_t org_addr;
static char current_file[256];
static int current_line;

static void die(const char *msg) {
    fprintf(stderr, "%s:%d: error: %s\n", current_file, current_line, msg);
    exit(1);
}

static void dief(const char *fmt, const char *arg) {
    fprintf(stderr, "%s:%d: error: ", current_file, current_line);
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n");
    exit(1);
}

/* Strip comments and trim */
static char *clean_line(const char *line_in) {
    static char buf[MAX_LINE];
    char *line = buf;
    strncpy(line, line_in, MAX_LINE - 1);
    line[MAX_LINE - 1] = '\0';
    char *p = strchr(line, ';');
    if (p) *p = '\0';
    while (*line && isspace(*line)) line++;
    char *end = line + strlen(line) - 1;
    while (end > line && isspace(*end)) *end-- = '\0';
    return line;
}

/* Parse register name → index */
static int parse_reg(const char *s) {
    if (toupper(s[0]) == 'R' && isdigit(s[1])) {
        int n = atoi(s + 1);
        if (n >= 0 && n < 64) return n;
    }
    if (toupper(s[0]) == 'F' && toupper(s[1]) == 'P' && isdigit(s[2])) {
        int n = atoi(s + 2);
        if (n >= 0 && n < 16) return 32 + n;
    }
    return -1;
}

/* Parse immediate: decimal, hex, binary */
static int32_t parse_imm(const char *s, char **endp) {
    char *ep;
    long v;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        v = strtol(s, &ep, 16);
    } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        v = strtol(s + 2, &ep, 2);
    } else {
        v = strtol(s, &ep, 10);
    }
    if (endp) *endp = ep;
    return (int32_t)v;
}

/* Find or add label */
static int find_label(const char *name) {
    for (int i = 0; i < label_count; i++)
        if (strcmp(labels[i].name, name) == 0) return i;
    return -1;
}

static void add_label(const char *name, uint32_t addr) {
    if (label_count >= MAX_LABELS) die("too many labels");
    if (find_label(name) >= 0) dief("duplicate label '%s'", name);
    strncpy(labels[label_count].name, name, 63);
    labels[label_count].addr = addr;
    label_count++;
}

static void add_fixup(uint32_t addr, const char *label) {
    if (fixup_count >= MAX_FIXUPS) die("too many fixups");
    strncpy(fixups[fixup_count].label, label, 63);
    fixups[fixup_count].patch_addr = addr;
    fixups[fixup_count].line = current_line;
    fixup_count++;
}

static void emit_byte(uint8_t b) {
    if (code_len >= MAX_CODE) die("code buffer overflow");
    code[code_len++] = b;
}

static void emit_reg(uint8_t r) { emit_byte(r); }

static void emit_imm16(int16_t v) {
    emit_byte((uint8_t)(v & 0xFF));
    emit_byte((uint8_t)((v >> 8) & 0xFF));
}

/* Tokenize a line into tokens separated by commas/spaces */
static int tokenize(char *line, char *tokens[], int max_tokens) {
    int count = 0;
    char *p = line;
    while (*p && count < max_tokens) {
        while (*p && (*p == ',' || *p == '\t' || *p == ' ')) p++;
        if (!*p) break;
        tokens[count++] = p;
        while (*p && *p != ',' && *p != '\t' && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    return count;
}

/* Parse opcode string → opcode byte, returns -1 if unknown */
typedef struct {
    const char *name;
    uint8_t op;
    int type; /* 0=no operands, 1=rd, 2=rd,rs, 3=rd,imm16, 4=rd,[rs], 5=label, 6=rd,label */
} OpInfo;

static const OpInfo op_table[] = {
    {"NOP",    OP_NOP,   0},
    {"MOV",    OP_MOV,   2},
    {"MOVI",   OP_MOVI,  3},
    {"LOAD",   OP_LOAD,  4},  /* special handling */
    {"STORE",  OP_STORE, 4},  /* special handling */
    {"ADD",    OP_ADD,   2},
    {"SUB",    OP_SUB,   2},
    {"MUL",    OP_MUL,   2},
    {"DIV",    OP_DIV,   2},
    {"MOD",    OP_MOD,   2},
    {"NEG",    OP_NEG,   1},
    {"INC",    OP_INC,   1},
    {"DEC",    OP_DEC,   1},
    {"AND",    OP_AND,   2},
    {"OR",     OP_OR,    2},
    {"XOR",    OP_XOR,   2},
    {"NOT",    OP_NOT,   1},
    {"SHL",    OP_SHL,   2},
    {"SHR",    OP_SHR,   2},
    {"ROTL",   OP_ROTL,  2},
    {"ROTR",   OP_ROTR,  2},
    {"CMP",    OP_CMP,   2},
    {"EQ",     OP_EQ,    2},
    {"NE",     OP_NE,    2},
    {"LT",     OP_LT,    2},
    {"LE",     OP_LE,    2},
    {"GT",     OP_GT,    2},
    {"GE",     OP_GE,    2},
    {"TEST",   OP_TEST,  2},
    {"SETCC",  OP_SETCC, 2},
    {"PUSH",   OP_PUSH,  1},
    {"POP",    OP_POP,   1},
    {"DUP",    OP_DUP,   0},
    {"SWAP",   OP_SWAP,  0},
    {"ENTER",  OP_ENTER, 1},
    {"LEAVE",  OP_LEAVE, 1},
    {"RET",    OP_RET,   0},
    {"JMP",    OP_JMP,   5},  /* label only */
    {"JZ",     OP_JZ,    5},
    {"JNZ",    OP_JNZ,   5},
    {"JE",     OP_JE,    5},
    {"JNE",    OP_JNE,   5},
    {"JL",     OP_JL,    5},
    {"JGE",    OP_JGE,   5},
    {"JGT",    0xFF,    5},  /* pseudo: JZ .past; JGE .target; .past: */
    {"CALL",   OP_CALL,  5},
    {"CAST",   OP_CAST,  2},
    {"BOX",    OP_BOX,   2},
    {"UNBOX",  OP_UNBOX, 2},
    {"FADD",   OP_FADD,  2},
    {"FSUB",   OP_FSUB,  2},
    {"FMUL",   OP_FMUL,  2},
    {"FDIV",   OP_FDIV,  2},
    {"FNEG",   OP_FNEG,  1},
    {"FABS",   OP_FABS,  0},
    {"FEQ",    OP_FEQ,   2},
    {"FLT",    OP_FLT,   2},
    {"PUT",    OP_PUT,   1},
    {"GET",    OP_GET,   1},
    {"PRNT",   OP_PRNT,  1},
    {"SPAWN",  OP_SPAWN, 7},  /* FLUX: spawn len16 */
    {"WAIT",   OP_WAIT,  7},
    {"SEND",   OP_SEND,  7},
    {"RECV",   OP_RECV,  7},
    {"GAUGE",  OP_GAUGE, 7},
    {"ENERGY", OP_ENERGY,7},
    {"TRUST",  OP_TRUST, 7},
    {"HALT",   OP_HALT,  0},
    {"YIELD",  OP_YIELD, 0},
    {NULL, 0, 0}
};

static const OpInfo *find_op(const char *name) {
    for (int i = 0; op_table[i].name; i++)
        if (strcasecmp(name, op_table[i].name) == 0) return &op_table[i];
    return NULL;
}

/* Check if token is a label reference (.name) */
static int is_label_ref(const char *s) {
    return s[0] == '.';
}

/* Check if token is memory reference [something] */
static int is_mem_ref(const char *s) {
    return s[0] == '[';
}

/* Extract content from [content], returns pointer into static buffer */
static char *extract_mem(const char *s) {
    static char buf[128];
    const char *end = strchr(s, ']');
    if (!end) die("missing ']' in memory reference");
    int len = (int)(end - s - 1);
    if (len <= 0 || len >= (int)sizeof(buf)) die("invalid memory reference");
    strncpy(buf, s + 1, len);
    buf[len] = '\0';
    /* trim */
    while (*buf == ' ' || *buf == '\t') memmove(buf, buf+1, strlen(buf));
    char *e = buf + strlen(buf) - 1;
    while (e > buf && (*e == ' ' || *e == '\t')) *e-- = '\0';
    return buf;
}

/* Pass 1: collect labels, compute sizes, emit placeholder bytes */
static void pass1(FILE *f, const char *filename) {
    char line[MAX_LINE];
    strcpy(current_file, filename);
    current_line = 0;

    while (fgets(line, sizeof(line), f)) {
        current_line++;
        char *cleaned = clean_line(line);
        if (!*cleaned) continue;

        /* Label definition — must be .name but not a directive */
        if (cleaned[0] == '.' && !isspace(cleaned[1]) && cleaned[1] != '\0'
            && strncasecmp(cleaned, ".org", 4) != 0
            && strncasecmp(cleaned, ".word", 5) != 0
            && strncasecmp(cleaned, ".byte", 5) != 0
            && strncasecmp(cleaned, ".data", 5) != 0
            && strncasecmp(cleaned, ".export", 7) != 0
            && strncasecmp(cleaned, ".import", 7) != 0) {
            char label_name[64];
            char remainder[MAX_LINE];
            int i = 0;
            label_name[i++] = '.';  /* keep the dot */
            const char *p = cleaned + 1;
            while (*p && !isspace(*p) && *p != ':' && i < 63) label_name[i++] = *p++;
            label_name[i] = '\0';
            /* skip colon if present */
            if (*p == ':') p++;
            add_label(label_name, code_len + org_addr);
            strncpy(remainder, p, MAX_LINE - 1);
            remainder[MAX_LINE - 1] = '\0';
            cleaned = clean_line(remainder);
            if (!*cleaned) continue;
        }

        /* Directives */
        if (cleaned[0] == '.') {
            if (strncasecmp(cleaned, ".org", 4) == 0) {
                char *arg = cleaned + 4;
                while (*arg == ' ' || *arg == '\t') arg++;
                org_addr = (uint32_t)parse_imm(arg, NULL);
                continue;
            }
            if (strncasecmp(cleaned, ".word", 5) == 0) {
                char *arg = cleaned + 5;
                while (*arg == ' ' || *arg == '\t') arg++;
                int32_t val = parse_imm(arg, NULL);
                emit_byte((uint8_t)(val & 0xFF));
                emit_byte((uint8_t)((val >> 8) & 0xFF));
                emit_byte((uint8_t)((val >> 16) & 0xFF));
                emit_byte((uint8_t)((val >> 24) & 0xFF));
                continue;
            }
            if (strncasecmp(cleaned, ".byte", 5) == 0) {
                char *arg = cleaned + 5;
                while (*arg == ' ' || *arg == '\t') arg++;
                emit_byte((uint8_t)parse_imm(arg, NULL));
                continue;
            }
            if (strncasecmp(cleaned, ".data", 5) == 0) {
                char *arg = cleaned + 5;
                while (*arg == ' ' || *arg == '\t') arg++;
                if (*arg == '"') {
                    arg++;
                    while (*arg && *arg != '"') {
                        if (*arg == '\\') {
                            arg++;
                            switch (*arg) {
                                case 'n': emit_byte('\n'); break;
                                case 't': emit_byte('\t'); break;
                                case 'r': emit_byte('\r'); break;
                                case '\\': emit_byte('\\'); break;
                                case '"': emit_byte('"'); break;
                                case '0': emit_byte('\0'); break;
                                default: emit_byte(*arg); break;
                            }
                        } else {
                            emit_byte((uint8_t)*arg);
                        }
                        arg++;
                    }
                }
                continue;
            }
            if (strncasecmp(cleaned, ".export", 7) == 0) continue;
            if (strncasecmp(cleaned, ".import", 7) == 0) continue;
        }

        /* Instruction */
        char *tokens[8];
        int ntok = tokenize(cleaned, tokens, 8);
        if (ntok == 0) continue;

        const OpInfo *op = find_op(tokens[0]);
        if (!op) dief("unknown instruction '%s'", tokens[0]);

        switch (op->type) {
        case 0: /* no operands */
            emit_byte(op->op);
            break;
        case 1: { /* rd */
            if (ntok < 2) die("expected register operand");
            int r = parse_reg(tokens[1]);
            if (r < 0) dief("invalid register '%s'", tokens[1]);
            emit_byte(op->op);
            emit_reg((uint8_t)r);
            break;
        }
        case 2: { /* rd, rs */
            if (ntok < 3) die("expected two register operands");
            int rd = parse_reg(tokens[1]);
            int rs = parse_reg(tokens[2]);
            if (rd < 0) dief("invalid register '%s'", tokens[1]);
            if (rs < 0) dief("invalid register '%s'", tokens[2]);
            emit_byte(op->op);
            emit_reg((uint8_t)rd);
            emit_reg((uint8_t)rs);
            break;
        }
        case 3: { /* rd, imm16 or FLUX opcodes */
            if (ntok < 2) die("expected register and immediate");
            int rd = parse_reg(tokens[1]);
            if (rd < 0) dief("invalid register '%s'", tokens[1]);
            emit_byte(op->op);
            emit_reg((uint8_t)rd);
            /* Check for label ref */
            if (ntok >= 3 && is_label_ref(tokens[2])) {
                emit_imm16(0); /* placeholder */
                add_fixup(code_len - 2, tokens[2]);
            } else if (ntok >= 3) {
                emit_imm16((int16_t)parse_imm(tokens[2], NULL));
            } else {
                emit_imm16(0);
            }
            break;
        }
        case 7: { /* FLUX agent opcode: OPCODE len16 */
            emit_byte(op->op);
            int16_t len = 0;
            if (ntok >= 2) {
                if (is_label_ref(tokens[1])) {
                    die("FLUX opcodes don't support label references");
                } else {
                    len = (int16_t)parse_imm(tokens[1], NULL);
                }
            }
            emit_byte((uint8_t)(len & 0xFF));
            emit_byte((uint8_t)((len >> 8) & 0xFF));
            break;
        }
        case 4: { /* LOAD/STORE with special syntax */
            /* LOAD rd, [rs] or LOAD rd, [imm] */
            /* STORE rd, [rs] or STORE rd, [imm] */
            if (ntok < 3) die("expected register and memory reference");
            int rd = parse_reg(tokens[1]);
            if (rd < 0) dief("invalid register '%s'", tokens[1]);
            if (!is_mem_ref(tokens[2])) {
                /* LOAD Rd, imm → MOVI Rd, imm */
                if (op->op == OP_LOAD) {
                    emit_byte(OP_MOVI);
                    emit_reg((uint8_t)rd);
                    if (is_label_ref(tokens[2])) {
                        emit_imm16(0);
                        add_fixup(code_len - 2, tokens[2]);
                    } else {
                        emit_imm16((int16_t)parse_imm(tokens[2], NULL));
                    }
                } else {
                    die("STORE requires memory reference [addr]");
                }
                break;
            }
            char *mem = extract_mem(tokens[2]);
            int rs = parse_reg(mem);
            if (op->op == OP_LOAD) {
                if (rs >= 0) {
                    emit_byte(OP_LOAD);
                    emit_reg((uint8_t)rd);
                    emit_reg((uint8_t)rs);
                } else {
                    /* LOAD Rd, [imm] → MOVI R31, imm; LOAD Rd, R31 */
                    emit_byte(OP_MOVI);
                    emit_reg(31); /* temp register */
                    if (is_label_ref(mem)) {
                        emit_imm16(0);
                        add_fixup(code_len - 2, mem);
                    } else {
                        emit_imm16((int16_t)parse_imm(mem, NULL));
                    }
                    emit_byte(OP_LOAD);
                    emit_reg((uint8_t)rd);
                    emit_reg(31);
                }
            } else { /* STORE */
                if (rs >= 0) {
                    /* STORE Rd, [Rs] → VM format: STORE Rs, Rd (addr reg first) */
                    emit_byte(OP_STORE);
                    emit_reg((uint8_t)rs);
                    emit_reg((uint8_t)rd);
                } else {
                    /* STORE Rd, [imm] → MOVI R31, imm; STORE R31, Rd */
                    emit_byte(OP_MOVI);
                    emit_reg(31);
                    if (is_label_ref(mem)) {
                        emit_imm16(0);
                        add_fixup(code_len - 2, mem);
                    } else {
                        emit_imm16((int16_t)parse_imm(mem, NULL));
                    }
                    emit_byte(OP_STORE);
                    emit_reg(31);
                    emit_reg((uint8_t)rd);
                }
            }
            break;
        }
        case 5: { /* label jump: JMP/JZ/etc label */
            if (ntok < 2) die("expected label");
            uint8_t jump_op = op->op;

            if (op->op == 0xFF) {
                /* JGT pseudo-op: JZ .past; JGE .target; .past: */
                emit_byte(OP_JZ);
                emit_reg(0); /* doesn't matter for flag jumps, but VM reads it */
                emit_imm16(3); /* skip over JGE + imm16 = 3 bytes */
                emit_byte(OP_JGE);
                emit_reg(0);
                emit_imm16(0); /* placeholder */
                add_fixup(code_len - 2, tokens[1]);
                break;
            }

            emit_byte(jump_op);
            emit_reg(0); /* VM reads rd byte even for flag-based jumps */
            emit_imm16(0); /* placeholder */
            add_fixup(code_len - 2, tokens[1]);
            break;
        }
        case 6: { /* rd, label — not used currently */
            break;
        }
        }
    }
}

/* Pass 2: resolve fixups */
static void pass2(void) {
    for (int i = 0; i < fixup_count; i++) {
        int li = find_label(fixups[i].label);
        if (li < 0) {
            fprintf(stderr, "%s:%d: error: undefined label '%s'\n",
                    current_file, fixups[i].line, fixups[i].label);
            exit(1);
        }
        uint32_t target = labels[li].addr;
        uint32_t patch = fixups[i].patch_addr;
        /* Compute relative offset: target - (patch + 2) since PC is after the imm16 */
        int32_t rel = (int32_t)target - (int32_t)(patch + 2);
        if (rel < -32768 || rel > 32767) {
            fprintf(stderr, "%s:%d: error: jump to '%s' out of range\n",
                    current_file, fixups[i].line, fixups[i].label);
            exit(1);
        }
        code[patch] = (uint8_t)(rel & 0xFF);
        code[patch + 1] = (uint8_t)((rel >> 8) & 0xFF);
    }
}

static void write_symfile(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    for (int i = 0; i < label_count; i++)
        fprintf(f, "%s 0x%04x\n", labels[i].name, labels[i].addr);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: flux-asm input.asm [-o output.bin] [-s output.sym]\n");
        return 1;
    }

    const char *infile = argv[1];
    const char *outfile = "a.out.bin";
    const char *symfile = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outfile = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) symfile = argv[++i];
    }

    FILE *f = fopen(infile, "r");
    if (!f) { perror(infile); return 1; }

    code_len = 0;
    org_addr = 0;
    label_count = 0;
    fixup_count = 0;

    pass1(f, infile);
    fclose(f);

    pass2();

    f = fopen(outfile, "wb");
    if (!f) { perror(outfile); return 1; }
    fwrite(code, 1, code_len, f);
    fclose(f);

    fprintf(stderr, "Assembled %u bytes → %s\n", code_len, outfile);

    if (symfile) write_symfile(symfile);

    return 0;
}
