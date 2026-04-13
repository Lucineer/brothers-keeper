#include <stdio.h>
extern int keeper_client_test(void);
extern int jt_power_thermal_test(void);
extern int jt_mem_tracker_test(void);
extern int jt_edge_net_test(void);
extern int jt_gpu_governor_test(void);
extern int jt_stream_scheduler_test(void);

typedef struct { const char *name; int (*test)(void); } TestSuite;
static const TestSuite suites[] = {
    {"Keeper Client SDK",      keeper_client_test},
    {"Power & Thermal",        jt_power_thermal_test},
    {"Memory Tracker",         jt_mem_tracker_test},
    {"Edge Networking",        jt_edge_net_test},
    {"GPU Governor",           jt_gpu_governor_test},
    {"Stream Scheduler",       jt_stream_scheduler_test},
};
#define N (sizeof(suites)/sizeof(suites[0]))

int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  Brothers-Keeper Library — Full Test Suite   ║\n");
    printf("║  Jetson Orin Nano 8GB — Real Hardware       ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    int total = 0, failures = 0;
    for (int i = 0; i < (int)N; i++) {
        printf("── %s ──\n", suites[i].name);
        int f = suites[i].test();
        printf("  %s\n\n", f ? "❌ FAILED" : "✅ PASSED");
        total++; failures += f;
    }

    printf("══════════════════════════════════════════════\n");
    printf("  %d/%d PASSED", total - failures, total);
    if (failures) printf(" (%d failures)", failures);
    printf("\n══════════════════════════════════════════════\n");
    return failures;
}
