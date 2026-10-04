#ifndef TCI_STREAM_H
#define TCI_STREAM_H

#include <stdint.h>

/*
 * On-wire TCI binary IQ/audio frame. This is the layout tci_client.c already
 * parses from a radio: a 64-byte header followed by interleaved float32
 * samples (I,Q for type 0, or L,R for type 1).
 *
 * A training file is a TCP TCI byte stream: semicolon-terminated text
 * commands (dds, vfo, cw_train, ...) then a sequence of these frames.
 */

#define TCI_STREAM_HEADER_SIZE 64
#define TCI_STREAM_IQ 0
#define TCI_STREAM_RX_AUDIO 1
#define TCI_AUDIO_FORMAT_FLOAT32 3
#define TCI_AUDIO_CHANNELS 2

typedef struct {
    uint32_t receiver;
    uint32_t sample_rate;
    uint32_t format;
    uint32_t codec;
    uint32_t crc;
    uint32_t length;    /* number of float32 values, not bytes */
    uint32_t type;      /* TCI_STREAM_IQ or TCI_STREAM_RX_AUDIO */
    uint32_t channels;
    uint32_t reserv[8];
} tci_stream_header_t;

#endif
