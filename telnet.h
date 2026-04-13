#ifndef TELNET_H
#define TELNET_H

#include <stddef.h>
#include <stdint.h>

/* Telnet command bytes */
#define TEL_IAC    0xFF
#define TEL_WILL   0xFB
#define TEL_WONT   0xFC
#define TEL_DO     0xFD
#define TEL_DONT   0xFE
#define TEL_SB     0xFA
#define TEL_SE     0xF0

/* Telnet option codes */
#define TEL_OPT_ECHO             1
#define TEL_OPT_SUPPRESS_GO_AHEAD 3
#define TEL_OPT_TERMINAL_TYPE   24
#define TEL_OPT_NAWS            31
#define TEL_OPT_MCCP2           86

/* Telnet subnegotiation: TERMINAL_TYPE SEND/IS */
#define TEL_TTYPE_SEND  1
#define TEL_TTYPE_IS    0

typedef enum {
    TELNET_STATE_NORMAL,
    TELNET_STATE_IAC_RECEIVED,
    TELNET_STATE_OPTION_NEGOTIATION,
    TELNET_STATE_SB_RECEIVED,
    TELNET_STATE_SB_DATA
} TelnetState;

typedef enum {
    COLOR_NONE,
    COLOR_ANSI_16,
    COLOR_ANSI_256
} ColorMode;

typedef struct {
    TelnetState state;
    int pending_option;  /* option byte after IAC+WILL/WONT/DO/DONT */
    int sb_option;       /* subnegotiation option */
    int sb_buf[64];      /* subneg data buffer */
    int sb_pos;
    ColorMode color_mode;
    int terminal_width;
    int terminal_height;
    char terminal_type[64];
    int mccp_enabled;
    int color_capable;   /* set to 1 if we've done TTYPE negotiation */
} TelnetConfig;

void telnet_init(TelnetConfig *cfg);
/* Process raw network input, strip IAC sequences, write clean text to out.
   Returns number of bytes written to out. Sends negotiation responses via fd. */
int telnet_process_input(TelnetConfig *cfg, int fd,
                         const uint8_t *buf, size_t len,
                         char *out, size_t out_size);

/* ANSI color/format helpers — return pointer to static buffer */
const char *telnet_color_bold(void);
const char *telnet_color_cyan(void);
const char *telnet_color_red(void);
const char *telnet_color_yellow(void);
const char *telnet_color_green(void);
const char *telnet_color_reset(void);

/* Formatted output — return pointer to static buffer */
const char *telnet_format_room_name(const char *name);
const char *telnet_format_exits(const char *exits);
/* level: 0=green, 1=yellow, 2=red */
const char *telnet_format_alert(const char *msg, int level);
/* gauge: name, current value, max value, unit label */
const char *telnet_format_gauge(const char *name, int value, int max, const char *unit);

#endif
