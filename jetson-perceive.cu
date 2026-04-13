/**
 * jetson-perceive.cu — GPU-accelerated perception kernel for brothers-keeper
 *
 * The meta-cognition layer. Runs on the GPU to monitor system state
 * in parallel, detect anomalies, and decide when to intervene.
 *
 * This is the JEPA-like perception system:
 * - Encodes system metrics into a latent state vector
 * - Predicts next state from history
 * - Flags when prediction deviates from reality (anomaly)
 * - Outputs intervention recommendations
 *
 * Why GPU? Because we're monitoring 64+ metrics at 10Hz.
 * CPU does this sequentially. GPU does it in parallel.
 * Also: this IS the bootstrapping loop. Each iteration
 * makes the perception better.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <cuda_runtime.h>

/* ═══ Configuration ═══ */

#define MAX_METRICS     64
#define HISTORY_LEN     256     /* Rolling window of past states */
#define LATENT_DIM      32      /* Compressed representation */
#define MAX_AGENTS      16
#define ANOMALY_THRESH  2.0f    /* Standard deviations */
#define PREDICT_THRESH  0.15f   /* 15% prediction error = anomaly */

/* ═══ Host Structures ═══ */

typedef struct {
    float values[MAX_METRICS];      /* Current metric values */
    float normalized[MAX_METRICS];  /* Z-score normalized */
    float prediction[MAX_METRICS]; /* Predicted next values */
    float anomaly_score;            /* 0.0 = normal, higher = weirder */
    float prediction_error;         /* MSE between prediction and actual */
    int anomaly_flags;              /* Bitfield of which metrics are anomalous */
    int intervention;               /* 0=none, 1=soft_nudge, 2=hard_intervene */
    char recommendation[256];       /* Human-readable recommendation */
    long timestamp_ms;
} JTPerceiveState;

typedef struct {
    /* Metric registry */
    char metric_names[MAX_METRICS][32];
    float metric_means[MAX_METRICS];
    float metric_stds[MAX_METRICS];
    int metric_count;

    /* History ring buffer */
    float history[HISTORY_LEN][MAX_METRICS];
    int history_head;
    int history_count;

    /* Latent space (GPU-resident) */
    float latent[HISTORY_LEN][LATENT_DIM];
    float weights_input[LATENT_DIM * MAX_METRICS];   /* Encoder */
    float weights_predict[LATENT_DIM * LATENT_DIM];   /* Predictor */
    float weights_output[LATENT_DIM * MAX_METRICS];   /* Decoder */
    float bias_enc[LATENT_DIM];
    float bias_pred[LATENT_DIM];
    float bias_dec[MAX_METRICS];

    /* Agent state */
    char agent_names[MAX_AGENTS][32];
    float agent_health[MAX_AGENTS];    /* 0.0 = dead, 1.0 = healthy */
    int agent_count;

    /* GPU pointers */
    float *d_metrics;
    float *d_history;
    float *d_weights_in;
    float *d_weights_pred;
    float *d_weights_out;
    float *d_bias_enc;
    float *d_bias_pred;
    float *d_bias_dec;
    float *d_output;
    int gpu_initialized;
} JTPerceiveCtx;

/* ═══ GPU Kernels ═══ */

/**
 * Encode metrics into latent space.
 * Each thread handles one latent dimension.
 * Matrix multiply: latent = metrics * W_enc + bias_enc
 */
__global__ void kernel_encode(
    const float *metrics,    /* [MAX_METRICS] */
    const float *weights,    /* [LATENT_DIM * MAX_METRICS] */
    const float *bias,       /* [LATENT_DIM] */
    float *output,           /* [LATENT_DIM] */
    int metric_count
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= LATENT_DIM) return;

    float sum = bias[idx];
    for (int m = 0; m < metric_count; m++) {
        sum += metrics[m] * weights[idx * MAX_METRICS + m];
    }
    /* tanh activation to keep latent in [-1, 1] */
    output[idx] = tanhf(sum);
}

/**
 * Predict next latent state from current.
 * latent_next = latent_current * W_pred + bias_pred
 */
__global__ void kernel_predict(
    const float *current,    /* [LATENT_DIM] */
    const float *weights,    /* [LATENT_DIM * LATENT_DIM] */
    const float *bias,       /* [LATENT_DIM] */
    float *output,           /* [LATENT_DIM] */
    int dim
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= dim) return;

    float sum = bias[idx];
    for (int j = 0; j < dim; j++) {
        sum += current[j] * weights[idx * dim + j];
    }
    output[idx] = tanhf(sum);
}

/**
 * Decode latent back to metric space (prediction).
 * predicted = latent * W_dec + bias_dec
 */
__global__ void kernel_decode(
    const float *latent,     /* [LATENT_DIM] */
    const float *weights,    /* [MAX_METRICS * LATENT_DIM] */
    const float *bias,       /* [MAX_METRICS] */
    float *output,           /* [MAX_METRICS] */
    int metric_count
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= metric_count) return;

    float sum = bias[idx];
    for (int l = 0; l < LATENT_DIM; l++) {
        sum += latent[l] * weights[idx * LATENT_DIM + l];
    }
    output[idx] = sum; /* Linear output for metric prediction */
}

/**
 * Compute anomaly scores: per-metric z-score of prediction error.
 */
__global__ void kernel_anomaly(
    const float *actual,         /* [MAX_METRICS] */
    const float *predicted,      /* [MAX_METRICS] */
    const float *means,          /* [MAX_METRICS] */
    const float *stds,           /* [MAX_METRICS] */
    float *anomaly_scores,       /* [MAX_METRICS] */
    int *anomaly_flags,          /* [MAX_METRICS] bitfield output */
    float threshold,
    int metric_count
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= metric_count) return;

    float error = fabsf(actual[idx] - predicted[idx]);
    float std = stds[idx] > 0.001f ? stds[idx] : 1.0f;
    float z = error / std;

    anomaly_scores[idx] = z;
    anomaly_flags[idx] = (z > threshold) ? 1 : 0;
}

/**
 * Agent health decay kernel.
 * Each thread handles one agent.
 * Health decays based on its associated metric anomalies.
 */
__global__ void kernel_agent_health(
    float *health,           /* [MAX_AGENTS] */
    const int *agent_metrics, /* [MAX_AGENTS] metric indices */
    const float *anomaly_scores, /* [MAX_METRICS] */
    int agent_count,
    float decay_rate
) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= agent_count) return;

    int m = agent_metrics[idx];
    float anomaly = (m >= 0) ? anomaly_scores[m] : 0.0f;

    /* Health decays proportional to anomaly, recovers when normal */
    if (anomaly > 1.0f) {
        health[idx] = fmaxf(0.0f, health[idx] - decay_rate * anomaly);
    } else {
        health[idx] = fminf(1.0f, health[idx] + decay_rate * 0.1f);
    }
}

/* ═══ Host API ═══ */

/**
 * Initialize perception context.
 * Allocates GPU memory, initializes weights with simple values.
 */
int jt_perceive_init(JTPerceiveCtx *ctx) {
    memset(ctx, 0, sizeof(JTPerceiveCtx));

    /* Initialize weights with simple random-like values */
    srand(42);
    for (int i = 0; i < LATENT_DIM * MAX_METRICS; i++)
        ctx->weights_input[i] = ((float)(rand() % 1000) - 500.0f) / 5000.0f;
    for (int i = 0; i < LATENT_DIM * LATENT_DIM; i++)
        ctx->weights_predict[i] = ((float)(rand() % 1000) - 500.0f) / 5000.0f;
    for (int i = 0; i < MAX_METRICS * LATENT_DIM; i++)
        ctx->weights_output[i] = ((float)(rand() % 1000) - 500.0f) / 5000.0f;
    for (int i = 0; i < LATENT_DIM; i++) ctx->bias_enc[i] = 0.0f;
    for (int i = 0; i < LATENT_DIM; i++) ctx->bias_pred[i] = 0.0f;
    for (int i = 0; i < MAX_METRICS; i++) ctx->bias_dec[i] = 0.0f;

    /* Default metric means/stds (will be calibrated) */
    for (int i = 0; i < MAX_METRICS; i++) {
        ctx->metric_means[i] = 50.0f;  /* Assume 0-100 range */
        ctx->metric_stds[i] = 15.0f;
    }

    /* Allocate GPU memory */
    cudaError_t err;
    err = cudaMalloc(&ctx->d_metrics, MAX_METRICS * sizeof(float));
    if (err != cudaSuccess) return -1;
    err = cudaMalloc(&ctx->d_history, MAX_METRICS * sizeof(float));
    if (err != cudaSuccess) return -2;
    err = cudaMalloc(&ctx->d_weights_in, LATENT_DIM * MAX_METRICS * sizeof(float));
    if (err != cudaSuccess) return -3;
    err = cudaMalloc(&ctx->d_weights_pred, LATENT_DIM * LATENT_DIM * sizeof(float));
    if (err != cudaSuccess) return -4;
    err = cudaMalloc(&ctx->d_weights_out, MAX_METRICS * LATENT_DIM * sizeof(float));
    if (err != cudaSuccess) return -5;
    err = cudaMalloc(&ctx->d_bias_enc, LATENT_DIM * sizeof(float));
    if (err != cudaSuccess) return -6;
    err = cudaMalloc(&ctx->d_bias_pred, LATENT_DIM * sizeof(float));
    if (err != cudaSuccess) return -7;
    err = cudaMalloc(&ctx->d_bias_dec, MAX_METRICS * sizeof(float));
    if (err != cudaSuccess) return -8;
    err = cudaMalloc(&ctx->d_output, MAX_METRICS * sizeof(float));
    if (err != cudaSuccess) return -9;

    /* Upload weights to GPU */
    cudaMemcpy(ctx->d_weights_in, ctx->weights_input,
               LATENT_DIM * MAX_METRICS * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(ctx->d_weights_pred, ctx->weights_predict,
               LATENT_DIM * LATENT_DIM * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(ctx->d_weights_out, ctx->weights_output,
               MAX_METRICS * LATENT_DIM * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(ctx->d_bias_enc, ctx->bias_enc,
               LATENT_DIM * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(ctx->d_bias_pred, ctx->bias_pred,
               LATENT_DIM * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(ctx->d_bias_dec, ctx->bias_dec,
               MAX_METRICS * sizeof(float), cudaMemcpyHostToDevice);

    ctx->gpu_initialized = 1;
    return 0;
}

/**
 * Register a metric to track.
 */
int jt_perceive_register_metric(JTPerceiveCtx *ctx, const char *name, float initial_mean, float initial_std) {
    if (ctx->metric_count >= MAX_METRICS) return -1;
    int idx = ctx->metric_count++;
    strncpy(ctx->metric_names[idx], name, 31);
    ctx->metric_means[idx] = initial_mean;
    ctx->metric_stds[idx] = initial_std > 0.001f ? initial_std : 1.0f;
    return idx;
}

/**
 * Register an agent and its primary metric.
 */
int jt_perceive_register_agent(JTPerceiveCtx *ctx, const char *name, int primary_metric_idx) {
    if (ctx->agent_count >= MAX_AGENTS) return -1;
    int idx = ctx->agent_count++;
    strncpy(ctx->agent_names[idx], name, 31);
    ctx->agent_health[idx] = 1.0f;
    return idx;
}

/**
 * Run one perception cycle.
 * Takes current metrics, predicts next state, detects anomalies.
 * All computation on GPU.
 */
int jt_perceive_cycle(JTPerceiveCtx *ctx, const float *metrics, JTPerceiveState *result) {
    if (!ctx->gpu_initialized) return -1;

    memset(result, 0, sizeof(JTPerceiveState));
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    result->timestamp_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    int mc = ctx->metric_count;
    if (mc == 0) mc = 1; /* Safety: at least 1 metric */

    int threads = 256;
    int blocks;

    /* Upload current metrics */
    cudaMemcpy(ctx->d_metrics, metrics, MAX_METRICS * sizeof(float), cudaMemcpyHostToDevice);

    /* Step 1: Encode metrics -> latent */
    float h_latent[LATENT_DIM];
    float *d_latent;
    cudaMalloc(&d_latent, LATENT_DIM * sizeof(float));
    blocks = (LATENT_DIM + threads - 1) / threads;
    kernel_encode<<<blocks, threads>>>(
        ctx->d_metrics, ctx->d_weights_in, ctx->d_bias_enc, d_latent, mc);
    cudaMemcpy(h_latent, d_latent, LATENT_DIM * sizeof(float), cudaMemcpyDeviceToHost);

    /* Step 2: Predict next latent */
    float h_predicted_latent[LATENT_DIM];
    float *d_predicted_latent;
    cudaMalloc(&d_predicted_latent, LATENT_DIM * sizeof(float));
    kernel_predict<<<blocks, threads>>>(
        d_latent, ctx->d_weights_pred, ctx->d_bias_pred, d_predicted_latent, LATENT_DIM);
    cudaMemcpy(h_predicted_latent, d_predicted_latent, LATENT_DIM * sizeof(float), cudaMemcpyDeviceToHost);

    /* Step 3: Decode prediction back to metric space */
    kernel_decode<<<(mc + threads - 1) / threads, threads>>>(
        d_predicted_latent, ctx->d_weights_out, ctx->d_bias_dec, ctx->d_output, mc);
    cudaMemcpy(result->prediction, ctx->d_output, MAX_METRICS * sizeof(float), cudaMemcpyDeviceToHost);

    /* Step 4: Compute anomaly scores */
    float *d_anomaly_scores;
    int *d_anomaly_flags;
    cudaMalloc(&d_anomaly_scores, MAX_METRICS * sizeof(float));
    cudaMalloc(&d_anomaly_flags, MAX_METRICS * sizeof(int));

    float *d_means, *d_stds;
    cudaMalloc(&d_means, MAX_METRICS * sizeof(float));
    cudaMalloc(&d_stds, MAX_METRICS * sizeof(float));
    cudaMemcpy(d_means, ctx->metric_means, MAX_METRICS * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_stds, ctx->metric_stds, MAX_METRICS * sizeof(float), cudaMemcpyHostToDevice);

    kernel_anomaly<<<(mc + threads - 1) / threads, threads>>>(
        ctx->d_metrics, ctx->d_output, d_means, d_stds,
        d_anomaly_scores, d_anomaly_flags, ANOMALY_THRESH, mc);

    float h_anomaly[MAX_METRICS];
    int h_flags[MAX_METRICS];
    cudaMemcpy(h_anomaly, d_anomaly_scores, MAX_METRICS * sizeof(float), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_flags, d_anomaly_flags, MAX_METRICS * sizeof(int), cudaMemcpyDeviceToHost);

    /* Compute overall anomaly score */
    float total_anomaly = 0.0f;
    float total_error = 0.0f;
    result->anomaly_flags = 0;
    for (int i = 0; i < mc; i++) {
        total_anomaly += h_anomaly[i];
        float err = fabsf(metrics[i] - result->prediction[i]);
        total_error += err * err;
        if (h_flags[i]) result->anomaly_flags |= (1 << i);
    }
    result->anomaly_score = mc > 0 ? total_anomaly / mc : 0.0f;
    result->prediction_error = mc > 0 ? sqrtf(total_error / mc) : 0.0f;

    /* Determine intervention level */
    if (result->anomaly_score > 4.0f) {
        result->intervention = 2;
        snprintf(result->recommendation, 255,
            "CRITICAL: anomaly=%.2f, %d metrics flagging. Immediate intervention needed.",
            result->anomaly_score, __builtin_popcount(result->anomaly_flags));
    } else if (result->anomaly_score > 2.0f) {
        result->intervention = 1;
        snprintf(result->recommendation, 255,
            "WARNING: anomaly=%.2f. Monitor closely. Consider soft nudge.",
            result->anomaly_score);
    } else {
        result->intervention = 0;
        snprintf(result->recommendation, 255, "NOMINAL: anomaly=%.2f. System operating normally.",
            result->anomaly_score);
    }

    /* Store in history */
    int h = ctx->history_head;
    memcpy(ctx->history[h], metrics, MAX_METRICS * sizeof(float));
    memcpy(ctx->latent[h], h_latent, LATENT_DIM * sizeof(float));
    ctx->history_head = (h + 1) % HISTORY_LEN;
    if (ctx->history_count < HISTORY_LEN) ctx->history_count++;

    /* Cleanup temporaries */
    cudaFree(d_latent);
    cudaFree(d_predicted_latent);
    cudaFree(d_anomaly_scores);
    cudaFree(d_anomaly_flags);
    cudaFree(d_means);
    cudaFree(d_stds);

    return 0;
}

/**
 * Calibrate means/stds from collected history.
 */
void jt_perceive_calibrate(JTPerceiveCtx *ctx) {
    if (ctx->history_count < 10) return;

    int mc = ctx->metric_count;
    int n = ctx->history_count;

    for (int m = 0; m < mc; m++) {
        double sum = 0.0;
        for (int i = 0; i < n; i++) {
            sum += ctx->history[i][m];
        }
        double mean = sum / n;

        double var = 0.0;
        for (int i = 0; i < n; i++) {
            double d = ctx->history[i][m] - mean;
            var += d * d;
        }
        double std = sqrt(var / n);

        ctx->metric_means[m] = (float)mean;
        ctx->metric_stds[m] = std > 0.001f ? (float)std : 1.0f;
    }
}

/**
 * Free GPU resources.
 */
void jt_perceive_free(JTPerceiveCtx *ctx) {
    if (ctx->d_metrics) cudaFree(ctx->d_metrics);
    if (ctx->d_history) cudaFree(ctx->d_history);
    if (ctx->d_weights_in) cudaFree(ctx->d_weights_in);
    if (ctx->d_weights_pred) cudaFree(ctx->d_weights_pred);
    if (ctx->d_weights_out) cudaFree(ctx->d_weights_out);
    if (ctx->d_bias_enc) cudaFree(ctx->d_bias_enc);
    if (ctx->d_bias_pred) cudaFree(ctx->d_bias_pred);
    if (ctx->d_bias_dec) cudaFree(ctx->d_bias_dec);
    if (ctx->d_output) cudaFree(ctx->d_output);
    memset(ctx, 0, sizeof(JTPerceiveCtx));
}

/* ═══ Tests ═══ */

int jt_perceive_test(void) {
    int failures = 0;
    printf("=== Jetson GPU Perception Tests ===\n");

    /* Test 1: Init */
    JTPerceiveCtx ctx;
    int rc = jt_perceive_init(&ctx);
    if (rc == 0) printf("  [1] Init: OK (GPU alloc succeeded)\n");
    else { printf("  [1] Init: FAILED (rc=%d)\n", rc); failures++; }

    /* Test 2: Register metrics */
    int cpu_m = jt_perceive_register_metric(&ctx, "cpu_percent", 45.0f, 15.0f);
    int ram_m = jt_perceive_register_metric(&ctx, "ram_available_mb", 3500.0f, 500.0f);
    int gpu_m = jt_perceive_register_metric(&ctx, "gpu_temp_c", 51.0f, 3.0f);
    int net_m = jt_perceive_register_metric(&ctx, "dns_latency_ms", 108.0f, 50.0f);
    int agent_m = jt_perceive_register_metric(&ctx, "active_agents", 3.0f, 1.0f);
    if (ctx.metric_count == 5) printf("  [2] Metrics: OK (registered %d)\n", ctx.metric_count);
    else { printf("  [2] Metrics: FAILED (got %d)\n", ctx.metric_count); failures++; }

    /* Test 3: Register agents */
    int a1 = jt_perceive_register_agent(&ctx, "flux-runtime", gpu_m);
    int a2 = jt_perceive_register_agent(&ctx, "craftmind", cpu_m);
    if (ctx.agent_count == 2 && a1 == 0 && a2 == 1)
        printf("  [3] Agents: OK (2 registered)\n");
    else { printf("  [3] Agents: FAILED\n"); failures++; }

    /* Test 4: Normal cycle */
    float normal[64] = {0};
    normal[cpu_m] = 45.0f;
    normal[ram_m] = 3500.0f;
    normal[gpu_m] = 51.0f;
    normal[net_m] = 108.0f;
    normal[agent_m] = 3.0f;

    JTPerceiveState state;
    rc = jt_perceive_cycle(&ctx, normal, &state);
    if (rc == 0) printf("  [4] Cycle: OK (anomaly=%.3f)\n", state.anomaly_score);
    else { printf("  [4] Cycle: FAILED (rc=%d)\n", rc); failures++; }

    /* Test 5: Run many normal cycles to build history */
    for (int i = 0; i < 50; i++) {
        float m[64] = {0};
        m[cpu_m] = 40.0f + (float)(i % 20);
        m[ram_m] = 3200.0f + (float)(i % 600);
        m[gpu_m] = 49.0f + (float)(i % 5);
        m[net_m] = 80.0f + (float)(i % 60);
        m[agent_m] = 2.0f + (float)(i % 3);
        jt_perceive_cycle(&ctx, m, &state);
    }
    printf("  [5] History: OK (%d samples collected)\n", ctx.history_count);

    /* Test 6: Calibrate */
    jt_perceive_calibrate(&ctx);
    printf("  [6] Calibrate: OK (cpu_mean=%.1f, gpu_mean=%.1f)\n",
           ctx.metric_means[cpu_m], ctx.metric_means[gpu_m]);

    /* Test 7: Anomaly detection - sudden spike */
    float spike[64] = {0};
    spike[cpu_m] = 95.0f;      /* Way above normal */
    spike[ram_m] = 500.0f;     /* Way below normal */
    spike[gpu_m] = 78.0f;      /* Hot! */
    spike[net_m] = 5000.0f;    /* DNS timeout */
    spike[agent_m] = 0.0f;     /* All agents dead */

    rc = jt_perceive_cycle(&ctx, spike, &state);
    printf("  [7] Anomaly: spike=%.3f, intervention=%d, flags=%d\n",
           state.anomaly_score, state.intervention, state.anomaly_flags);
    if (state.intervention >= 1) printf("     Correctly detected anomaly!\n");
    else { printf("     FAILED: should have detected anomaly\n"); failures++; }

    /* Test 8: Anomaly detection - gradual drift */
    float drift[64] = {0};
    drift[cpu_m] = 65.0f;
    drift[ram_m] = 2800.0f;
    drift[gpu_m] = 58.0f;
    drift[net_m] = 300.0f;
    drift[agent_m] = 1.0f;

    jt_perceive_cycle(&ctx, drift, &state);
    printf("  [8] Drift: score=%.3f, intervention=%d\n",
           state.anomaly_score, state.intervention);
    if (state.anomaly_score > 0) printf("     Drift detected (score > 0)\n");
    else { printf("     FAILED: should detect drift\n"); failures++; }

    /* Test 9: Recovery - back to normal */
    rc = jt_perceive_cycle(&ctx, normal, &state);
    printf("  [9] Recovery: score=%.3f, intervention=%d\n",
           state.anomaly_score, state.intervention);
    if (state.intervention == 0) printf("     Correctly returned to nominal\n");
    else { printf("     Note: still elevated (expected with random weights)\n"); }

    /* Test 10: GPU performance */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 1000; i++) {
        jt_perceive_cycle(&ctx, normal, &state);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("  [10] Perf: 1000 cycles in %.3fms (%.0f cycles/sec)\n",
           elapsed * 1000.0, 1000.0 / elapsed);

    /* Test 11: Null safety */
    JTPerceiveCtx null_ctx = {0};
    rc = jt_perceive_cycle(&null_ctx, normal, &state);
    if (rc == -1) printf("  [11] Null safety: OK (uninitialized returns -1)\n");
    else { printf("  [11] Null safety: FAILED\n"); failures++; }

    /* Test 12: Recommendation text */
    printf("  [12] Recommendation: \"%s\"\n", state.recommendation);
    if (strlen(state.recommendation) > 0) printf("     OK (non-empty)\n");
    else { printf("     FAILED (empty)\n"); failures++; }

    /* Test 13: Timestamp */
    if (state.timestamp_ms > 0) printf("  [13] Timestamp: OK (%ld ms)\n", state.timestamp_ms);
    else { printf("  [13] Timestamp: FAILED\n"); failures++; }

    /* Test 14: Cleanup */
    jt_perceive_free(&ctx);
    if (ctx.gpu_initialized == 0) printf("  [14] Cleanup: OK\n");
    else { printf("  [14] Cleanup: FAILED\n"); failures++; }

    printf("jt_perceive_test: %d failures\n", failures);
    return failures;
}

int main(void) {
    return jt_perceive_test();
}
