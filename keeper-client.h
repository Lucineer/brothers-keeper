/**
 * keeper-client.h — C SDK for bare-metal GPU agents to get keys from brothers-keeper
 *
 * An agent on the Jetson GPU doesn't need to know API keys.
 * It authenticates with the keeper, gets a session, requests keys.
 * Budget is tracked automatically.
 *
 * Usage:
 *   KeeperClient client;
 *   keeper_init(&client, "my-experiment-1", "agent_auth_token");
 *   char key[256];
 *   int rc = keeper_get_key(&client, "deepinfra", key, sizeof(key));
 *   // key now has the API key — use it, don't store it
 *   keeper_report_usage(&client, "deepinfra", 0.0005);
 */

#ifndef KEEPER_CLIENT_H
#define KEEPER_CLIENT_H

#include <stdint.h>

#define KEEPER_DEFAULT_PORT  9437
#define KEEPER_HOST          "127.0.0.1"
#define KEEPER_MAX_KEY_LEN   512
#define KEEPER_MAX_ERROR     256
#define KEEPER_SESSION_LEN   32

typedef struct {
    char agent_name[64];
    char auth_token[64];
    char session[KEEPER_SESSION_LEN];
    float daily_limit_usd;
    float used_today_usd;
    float remaining_usd;
    int   authenticated;
    char  last_error[KEEPER_MAX_ERROR];
} KeeperClient;

/** Initialize client with agent identity */
int keeper_init(KeeperClient *client, const char *agent_name, const char *auth_token);

/** Authenticate with keeper, get session */
int keeper_authenticate(KeeperClient *client);

/** Request API key from keeper */
int keeper_get_key(KeeperClient *client, const char *provider,
                   char *key_out, int key_max_len);

/** Request API key with cost estimate for budget check */
int keeper_get_key_budgeted(KeeperClient *client, const char *provider,
                            char *key_out, int key_max_len,
                            float estimated_cost_usd);

/** Report usage after API call */
int keeper_report_usage(KeeperClient *client, const char *provider,
                        float cost_usd);

/** Check remaining budget */
int keeper_check_budget(KeeperClient *client);

/** Register a new agent with the keeper */
int keeper_register(const char *agent_name, float daily_limit_usd,
                    char *auth_token_out, int token_max_len);

/** Test the SDK */
int keeper_client_test(void);

#endif
