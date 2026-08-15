#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    char radio_host[256];
    int radio_port;
    char radio_protocol[32];  /* "tcp" or "websocket" */
    float center_frequency;
    int sample_rate;
    
    char spot_server_host[256];
    int spot_server_port;
    char spot_server_callsign[64];
    
    int detection_threshold;  // 0-100 Bayesian confidence % (lower = more sensitive)
    float min_snr_db;         // Minimum dB above noise floor to list a signal
    float decode_min_snr_db;  // Minimum dB above noise floor to open a decode channel
    float decode_bucket_hz;   // Merge decode channels within this Hz width (default 1000)
    char validation_mode[16]; // minimal, normal, aggressive, paranoid
    int spot_enabled;         // 0=disabled, 1=enable spot network reporting
    int log_level;            // 0=debug, 1=info, 2=warn, 3=error
    char log_file[256];
    char deepcw_model_path[512];
    /* Spectrum span in Hz: 0 or sample_rate = full 48 kHz (default/working).
     * 3000 = experimental ±1.5 kHz around VFO, finer bins / faster hop. */
    int spectrum_span_hz;
    /* TCI binary stream: "iq" (complex baseband) or "audio" (demod L/R). */
    char tci_stream_mode[16];
    /* Parallel CW Decode channels (GUI multi-channel Morse). */
    int multi_decode_channels;
} config_t;

/**
 * Load configuration from INI file
 * @param path Path to config file
 * @param config Pointer to config struct to populate
 * @return 0 on success, -1 on error
 */
int config_load(const char *path, config_t *config);

/**
 * Load config from an explicit path, or search common locations.
 * @param explicit_path Optional path (NULL to search automatically)
 * @return 0 on success, -1 if no config file was found
 */
int config_load_auto(const char *explicit_path, config_t *config);

/**
 * Set default configuration values
 */
void config_defaults(config_t *config);

#endif
