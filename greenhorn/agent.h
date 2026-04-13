/*
 * Greenhorn FLUX Agent — lifecycle: boot, execute, shutdown
 */
#ifndef GREENHORN_AGENT_H
#define GREENHORN_AGENT_H

#include "vm.h"

typedef struct {
    GHVM vm;
    const char *name;
    uint32_t max_cycles;
} GHAgent;

int  gh_agent_boot(GHAgent *a, const char *name, const uint8_t *bytecode, uint32_t len);
int64_t gh_agent_run(GHAgent *a);
void gh_agent_shutdown(GHAgent *a);

#endif
