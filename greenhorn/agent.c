#include "agent.h"
#include <string.h>
#include <stdio.h>

int gh_agent_boot(GHAgent *a, const char *name, const uint8_t *bytecode, uint32_t len) {
    memset(a, 0, sizeof(*a));
    a->name = name;
    a->max_cycles = 10000000;
    gh_vm_load(&a->vm, bytecode, len);
    a->vm.max_cycles = a->max_cycles;
    return 0;
}

int64_t gh_agent_run(GHAgent *a) {
    return gh_vm_execute(&a->vm);
}

void gh_agent_shutdown(GHAgent *a) {
    memset(a, 0, sizeof(*a));
}
