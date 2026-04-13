/*
 * flux-link.c — Simple linker for Greenhorn FLUX VM bytecode modules
 * Usage: flux-link [-o output.bin] [-s output.sym] module1.bin [module2.bin ...]
 *
 * Each module can have an optional .sym file alongside it (module1.sym).
 * Symbols from .sym files are used for cross-module resolution.
 * Modules are concatenated with address relocation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_MODULES 64
#define MAX_SYMBOLS 4096
#define MAX_CODE    (64*1024)
#define MAX_RELOCS  8192

typedef struct {
    char name[64];
    uint32_t addr;
    int resolved;
} Symbol;

typedef struct {
    char name[64];
    uint32_t offset; /* offset within module where imm16 is */
    uint32_t module_id;
} Reloc;

static Symbol symbols[MAX_SYMBOLS];
static int symbol_count;

static uint8_t output[MAX_CODE];
static uint32_t out_len;

static int find_symbol(const char *name) {
    for (int i = 0; i < symbol_count; i++)
        if (strcmp(symbols[i].name, name) == 0) return i;
    return -1;
}

static void add_symbol(const char *name, uint32_t addr) {
    if (symbol_count >= MAX_SYMBOLS) { fprintf(stderr, "too many symbols\n"); exit(1); }
    int existing = find_symbol(name);
    if (existing >= 0) {
        symbols[existing].addr = addr;
        symbols[existing].resolved = 1;
        return;
    }
    strncpy(symbols[symbol_count].name, name, 63);
    symbols[symbol_count].addr = addr;
    symbols[symbol_count].resolved = 1;
    symbol_count++;
}

/* Load symbol file */
static void load_sym(const char *symfile, uint32_t base_offset) {
    FILE *f = fopen(symfile, "r");
    if (!f) return; /* no sym file is ok */
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char name[64];
        unsigned int addr;
        if (sscanf(line, "%63s 0x%x", name, &addr) == 2) {
            add_symbol(name, addr + base_offset);
        }
    }
    fclose(f);
}

static void make_sym_path(const char *binfile, char *symfile, int maxlen) {
    strncpy(symfile, binfile, maxlen - 1);
    char *dot = strrchr(symfile, '.');
    if (dot) strcpy(dot, ".sym");
    else strncat(symfile, ".sym", maxlen - strlen(symfile) - 1);
}

int main(int argc, char **argv) {
    const char *outfile = "a.out.bin";
    const char *symfile = NULL;
    int first_file_arg = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { outfile = argv[++i]; }
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) { symfile = argv[++i]; }
        else { first_file_arg = i; break; }
    }

    int nmodules = argc - first_file_arg;
    if (nmodules < 1) {
        fprintf(stderr, "Usage: flux-link [-o output.bin] [-s output.sym] module1.bin [module2.bin ...]\n");
        return 1;
    }

    out_len = 0;
    symbol_count = 0;

    /* Load all modules */
    for (int i = 0; i < nmodules; i++) {
        const char *modfile = argv[first_file_arg + i];
        FILE *f = fopen(modfile, "rb");
        if (!f) { perror(modfile); return 1; }

        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (out_len + len > MAX_CODE) {
            fprintf(stderr, "error: output would exceed %d bytes\n", MAX_CODE);
            return 1;
        }

        size_t nread = fread(output + out_len, 1, len, f);
        fclose(f);

        fprintf(stderr, "  + %s (%ld bytes) @ 0x%04x\n", modfile, nread, out_len);

        /* Load symbols */
        char spath[512];
        make_sym_path(modfile, spath, sizeof(spath));
        load_sym(spath, out_len);

        out_len += (uint32_t)nread;
    }

    /* Write output */
    FILE *f = fopen(outfile, "wb");
    if (!f) { perror(outfile); return 1; }
    fwrite(output, 1, out_len, f);
    fclose(f);

    fprintf(stderr, "Linked %u bytes → %s\n", out_len, outfile);

    /* Write symbol table */
    if (symfile) {
        f = fopen(symfile, "w");
        if (!f) { perror(symfile); return 1; }
        for (int i = 0; i < symbol_count; i++)
            fprintf(f, "%s 0x%04x\n", symbols[i].name, symbols[i].addr);
        fclose(f);
        fprintf(stderr, "Symbols → %s (%d entries)\n", symfile, symbol_count);
    }

    return 0;
}
