#define _GNU_SOURCE
#include "telnet.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>

/* Send raw bytes to fd */
static void telnet_send(int fd, const uint8_t *data, size_t len) {
    (void)write(fd, data, len);
}

/* Send IAC WILL/WONT/DO/DONT option */
static void telnet_negotiate(int fd, uint8_t cmd, uint8_t option) {
    uint8_t buf[3] = {TEL_IAC, cmd, option};
    telnet_send(fd, buf, 3);
}

void telnet_init(TelnetConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->state = TELNET_STATE_NORMAL;
    cfg->terminal_width = 80;
    cfg->terminal_height = 24;
    cfg->color_mode = COLOR_ANSI_16;
}

int telnet_process_input(TelnetConfig *cfg, int fd,
                         const uint8_t *buf, size_t len,
                         char *out, size_t out_size) {
    size_t opos = 0;

    for (size_t i = 0; i < len && opos < out_size - 1; i++) {
        uint8_t ch = buf[i];

        switch (cfg->state) {
        case TELNET_STATE_NORMAL:
            if (ch == TEL_IAC) {
                cfg->state = TELNET_STATE_IAC_RECEIVED;
            } else {
                out[opos++] = (char)ch;
            }
            break;

        case TELNET_STATE_IAC_RECEIVED:
            switch (ch) {
            case TEL_IAC: /* escaped 0xFF */
                out[opos++] = (char)0xFF;
                cfg->state = TELNET_STATE_NORMAL;
                break;
            case TEL_WILL:
            case TEL_WONT:
            case TEL_DO:
            case TEL_DONT:
                cfg->pending_option = ch;
                cfg->state = TELNET_STATE_OPTION_NEGOTIATION;
                break;
            case TEL_SB:
                cfg->state = TELNET_STATE_SB_RECEIVED;
                break;
            case TEL_SE:
                /* stray SE, ignore */
                cfg->state = TELNET_STATE_NORMAL;
                break;
            default:
                /* other commands (NOP, GA, etc.) — skip */
                cfg->state = TELNET_STATE_NORMAL;
                break;
            }
            break;

        case TELNET_STATE_OPTION_NEGOTIATION:
            {
                uint8_t option = ch;
                uint8_t cmd = (uint8_t)cfg->pending_option;
                cfg->state = TELNET_STATE_NORMAL;

                switch (cmd) {
                case TEL_DO:
                    switch (option) {
                    case TEL_OPT_SUPPRESS_GO_AHEAD:
                        telnet_negotiate(fd, TEL_WILL, option);
                        break;
                    case TEL_OPT_TERMINAL_TYPE:
                        telnet_negotiate(fd, TEL_WILL, option);
                        /* request TTYPE after agreeing */
                        {
                            uint8_t sb[6] = {TEL_IAC, TEL_SB, TEL_OPT_TERMINAL_TYPE, TEL_TTYPE_SEND, TEL_IAC, TEL_SE};
                            telnet_send(fd, sb, 6);
                        }
                        break;
                    case TEL_OPT_NAWS:
                        /* client sends this via SB, just acknowledge */
                        telnet_negotiate(fd, TEL_WILL, option);
                        break;
                    case TEL_OPT_MCCP2:
                        telnet_negotiate(fd, TEL_WONT, option);
                        cfg->mccp_enabled = 0;
                        break;
                    default:
                        telnet_negotiate(fd, TEL_WONT, option);
                        break;
                    }
                    break;
                case TEL_DONT:
                    telnet_negotiate(fd, TEL_WONT, option);
                    break;
                case TEL_WILL:
                    switch (option) {
                    case TEL_OPT_ECHO:
                        telnet_negotiate(fd, TEL_DO, option);
                        break;
                    case TEL_OPT_SUPPRESS_GO_AHEAD:
                        telnet_negotiate(fd, TEL_DO, option);
                        break;
                    case TEL_OPT_MCCP2:
                        telnet_negotiate(fd, TEL_DONT, option);
                        cfg->mccp_enabled = 0;
                        break;
                    default:
                        telnet_negotiate(fd, TEL_DONT, option);
                        break;
                    }
                    break;
                case TEL_WONT:
                    telnet_negotiate(fd, TEL_DONT, option);
                    break;
                }
            }
            break;

        case TELNET_STATE_SB_RECEIVED:
            cfg->sb_option = ch;
            cfg->sb_pos = 0;
            cfg->state = TELNET_STATE_SB_DATA;
            break;

        case TELNET_STATE_SB_DATA:
            if (ch == TEL_IAC) {
                /* next byte should be SE */
                cfg->state = TELNET_STATE_NORMAL; /* will handle on next iter if SE */
            } else {
                if (cfg->sb_pos < 63) {
                    cfg->sb_buf[cfg->sb_pos++] = ch;
                }
            }
            /* check for SE via IAC+SE pattern */
            if (i + 1 < len && buf[i + 1] == TEL_SE && ch == TEL_IAC) {
                /* actually this is IAC, next is SE */
                i++; /* skip SE */
                /* process subneg */
                switch (cfg->sb_option) {
                case TEL_OPT_NAWS:
                    if (cfg->sb_pos >= 3) {
                        /* NAWS: IAC SB NAWS width_hi width_lo height_hi height_lo IAC SE */
                        /* sb_buf has data after option byte; but we stored from sb_received */
                        /* Actually we stored from after SB+option. NAWS is 4 bytes. */
                        /* sb_pos counts data bytes. For NAWS, client sends IAC SB NAWS <4 bytes> IAC SE */
                        /* We're in SB_DATA after SB_RECEIVED set sb_option.
                           But the IAC before SE was consumed, so sb_buf has the 4 NAWS bytes minus nothing.
                           Wait — the IAC before SE sets state back to NORMAL and we increment i.
                           So sb_buf has everything between SB+option and IAC SE. */
                        if (cfg->sb_pos >= 4) {
                            uint16_t w = (uint16_t)((cfg->sb_buf[0] << 8) | cfg->sb_buf[1]);
                            uint16_t h = (uint16_t)((cfg->sb_buf[2] << 8) | cfg->sb_buf[3]);
                            if (w > 0 && w < 512) cfg->terminal_width = w;
                            if (h > 0 && h < 256) cfg->terminal_height = h;
                        }
                    }
                    break;
                case TEL_OPT_TERMINAL_TYPE:
                    if (cfg->sb_pos > 1 && cfg->sb_buf[0] == TEL_TTYPE_IS) {
                        int slen = cfg->sb_pos - 1;
                        if (slen > 63) slen = 63;
                        memcpy(cfg->terminal_type, &cfg->sb_buf[1], slen);
                        cfg->terminal_type[slen] = '\0';
                        cfg->color_capable = 1;
                        /* detect xterm for 256-color */
                        if (strstr(cfg->terminal_type, "256color") ||
                            strstr(cfg->terminal_type, "xterm-256") ||
                            strstr(cfg->terminal_type, "screen-256")) {
                            cfg->color_mode = COLOR_ANSI_256;
                        }
                    }
                    break;
                }
                cfg->state = TELNET_STATE_NORMAL;
            } else if (ch != TEL_IAC) {
                /* normal SB data byte, already stored */
            }
            /* If we got IAC and next isn't SE, state is NORMAL but we already consumed IAC.
               This handles edge cases. */
            break;
        }
    }

    out[opos] = '\0';
    return (int)opos;
}

/* ANSI escape helpers */
static const char *ANSI_BOLD   = "\033[1m";
static const char *ANSI_CYAN   = "\033[36m";
static const char *ANSI_RED    = "\033[31m";
static const char *ANSI_YELLOW = "\033[33m";
static const char *ANSI_GREEN  = "\033[32m";
static const char *ANSI_RESET  = "\033[0m";

const char *telnet_color_bold(void)   { return ANSI_BOLD; }
const char *telnet_color_cyan(void)   { return ANSI_CYAN; }
const char *telnet_color_red(void)    { return ANSI_RED; }
const char *telnet_color_yellow(void) { return ANSI_YELLOW; }
const char *telnet_color_green(void)  { return ANSI_GREEN; }
const char *telnet_color_reset(void)  { return ANSI_RESET; }

static char fmt_buf[4096];

const char *telnet_format_room_name(const char *name) {
    snprintf(fmt_buf, sizeof(fmt_buf), "%s%s%s", ANSI_BOLD, ANSI_CYAN, name);
    size_t l = strlen(fmt_buf);
    snprintf(fmt_buf + l, sizeof(fmt_buf) - l, "%s", ANSI_RESET);
    return fmt_buf;
}

const char *telnet_format_exits(const char *exits) {
    snprintf(fmt_buf, sizeof(fmt_buf), "%sExits: %s%s", ANSI_CYAN, exits, ANSI_RESET);
    return fmt_buf;
}

const char *telnet_format_alert(const char *msg, int level) {
    const char *color = ANSI_GREEN;
    if (level >= 2) color = ANSI_RED;
    else if (level >= 1) color = ANSI_YELLOW;
    snprintf(fmt_buf, sizeof(fmt_buf), "%s%s%s", color, msg, ANSI_RESET);
    return fmt_buf;
}

const char *telnet_format_gauge(const char *name, int value, int max, const char *unit) {
    int pct = (max > 0) ? (value * 100 / max) : 0;
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;

    const char *color = ANSI_GREEN;
    if (pct < 25) color = ANSI_RED;
    else if (pct < 50) color = ANSI_YELLOW;

    int bar_len = 20;
    int filled = (bar_len * pct) / 100;
    char bar[21];
    for (int i = 0; i < bar_len; i++) {
        bar[i] = (i < filled) ? '#' : '-';
    }
    bar[bar_len] = '\0';

    snprintf(fmt_buf, sizeof(fmt_buf), "  %s%-12s%s [%s%s%s] %d/%d %s",
             ANSI_BOLD, name, ANSI_RESET,
             color, bar, ANSI_RESET,
             value, max, unit);
    return fmt_buf;
}
