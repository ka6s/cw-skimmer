#ifndef RADIO_CONTROL_H
#define RADIO_CONTROL_H

/*
 * Tune and CW text for Thetis and deskHPSDR.
 *
 * Frequency goes out on the TCI socket we already use. dds is sent first,
 * then vfo, both at the same Hz. Thetis moves the panadapter with dds and
 * then sets the dial with vfo. deskHPSDR has no dds setter; vfo recenters
 * the receiver.
 *
 * CW text defaults to TCI cw_macros, which current Thetis and deskHPSDR
 * both key. cw_send=ky uses the Kenwood TS-2000 "KY <text>;" command on
 * cat_host:cat_port (Thetis CAT, or deskHPSDR's rigctld bridge). The
 * built-in deskHPSDR TS-2000 port does not implement KY. cat_port 0 stays
 * on TCI.
 */

#include "config.h"
#include "tci_client.h"

typedef struct radio_control radio_control_t;

radio_control_t *radio_control_create(void);
void radio_control_destroy(radio_control_t *rc);
void radio_control_apply_config(radio_control_t *rc, const config_t *cfg);

int radio_control_tune(radio_control_t *rc, long long hz);
int radio_control_cw_text(radio_control_t *rc, const char *text);
int radio_control_cw_stop(radio_control_t *rc);
int radio_control_set_wpm(radio_control_t *rc, int wpm);

/** Drain queued commands. Call from the detection thread only. */
void radio_control_service(radio_control_t *rc, tci_client_t *tci);

/** "tci" or "ky" after config fallback. Stable until the next apply. */
const char *radio_control_send_mode(const radio_control_t *rc);

int radio_control_sanitize_cw(const char *in, char *out, int out_max);
int radio_control_format_tune(int rx, long long hz,
                              char *dds, int dds_max,
                              char *vfo, int vfo_max);
int radio_control_format_cw_macro(int rx, const char *text,
                                  char *cmd, int cmd_max);
int radio_control_format_wpm(int wpm,
                             char *keyer, int keyer_max,
                             char *macros, int macros_max);
/** 1 when a chunk was written. Advances *pos. Text is at most 24 characters. */
int radio_control_ky_chunk(const char *text, int *pos, char *cmd, int cmd_max);
/** 0 buffer free, 1 busy, -1 if the line is not a KY reply. */
int radio_control_ky_status(const char *line);

#endif
