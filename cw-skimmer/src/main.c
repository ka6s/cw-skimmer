#include "tci_client.h"
#include "config.h"
#include "logger.h"
#include "cw_detector.h"
#include "cw_decoder.h"
#include "spot_reporter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <time.h>

/* usleep declaration for strict C99 builds */
#if !defined(_BSD_SOURCE) && !defined(_DEFAULT_SOURCE) && !defined(_XOPEN_SOURCE)
int usleep(unsigned int __useconds);
#endif

static int running = 1;

void signal_handler(int sig) {
    (void)sig;
    running = 0;
    LOG_INFO("Shutdown signal received");
}

int main(int argc, char *argv[]) {
    config_t config;

    if (argc > 1) {
        config_load_auto(argv[1], &config);
    } else {
        config_load_auto(NULL, &config);
    }
    
    logger_init(config.log_level, config.log_file);
    LOG_INFO("CW Skimmer starting");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create TCI client
    tci_client_t *radio = tci_client_create(config.radio_host, config.radio_port,
                                            config.sample_rate, config.radio_protocol);
    if (!radio) {
        LOG_ERROR("Failed to create TCI client");
        return 1;
    }
    
    // Connect to radio
    if (tci_client_connect(radio) < 0) {
        LOG_ERROR("Failed to connect to radio");
        tci_client_destroy(radio);
        return 1;
    }
    
    // Subscribe to I/Q stream
    if (tci_subscribe_iq_stream(radio, 0) < 0) {
        LOG_ERROR("Failed to subscribe to I/Q stream");
        tci_client_destroy(radio);
        return 1;
    }
    
    // Create detector
    int num_channels = config.sample_rate / 100;  // ~100Hz per channel
    cw_detector_t *detector = cw_detector_create(config.sample_rate, num_channels, config.detection_threshold);
    if (!detector) {
        LOG_ERROR("Failed to create detector");
        tci_client_destroy(radio);
        return 1;
    }
    
    if (cw_decoder_global_init() != 0) {
        LOG_ERROR("Failed to initialize ditdah Morse decoder");
        cw_detector_destroy(detector);
        tci_client_destroy(radio);
        return 1;
    }

    cw_decoder_t *decoder = cw_decoder_create(config.sample_rate, 20);
    if (!decoder) {
        LOG_ERROR("Failed to create CW decoder");
        cw_decoder_global_shutdown();
        cw_detector_destroy(detector);
        tci_client_destroy(radio);
        return 1;
    }
    
    // Create spot reporter
    spot_reporter_t *reporter = spot_reporter_create(
        config.spot_server_host,
        config.spot_server_port,
        config.spot_server_callsign);
    if (!reporter) {
        LOG_ERROR("Failed to create spot reporter");
        cw_decoder_destroy(decoder);
        cw_detector_destroy(detector);
        tci_client_destroy(radio);
        return 1;
    }
    
    LOG_INFO("CW Detector initialized with %d channels", num_channels);
    
    // Processing loop
    float complex *iq_buffer = malloc(config.sample_rate * sizeof(float complex));
    int samples_processed = 0;
    int read_count = 0;
    float last_signal_freq = 0.0f;  /* for associating decoded callsigns */
    
    while (running) {
        tci_service_websocket();

        if (tci_is_connected(radio)) {
            // Try to read new I/Q samples
            int new_samples = tci_read_iq_samples(radio);
            if (new_samples > 0) {
                read_count++;
                samples_processed += new_samples;
            }
            
            // When buffer has enough samples, process them
            int available = tci_buffer_available(radio);
            if (available >= 1024) {
                int to_process = (available / 1024) * 1024;
                int got = tci_get_iq_samples(radio, iq_buffer, to_process);
                
                if (got > 0) {
                    cw_detector_analyze(detector, iq_buffer, got);
                    
                    // Get detected signals
                    cw_signal_t signals[10];
                    int num_signals = cw_detector_get_signals(detector, signals, 10);
                    
                    if (num_signals > 0) {
                        LOG_INFO("Detected %d signals, noise floor: %.1f dB", 
                                 num_signals, cw_detector_get_noise_floor(detector));
                        
                        for (int i = 0; i < num_signals; i++) {
                            LOG_INFO("  Signal %d: %.1f Hz, SNR: %.1f dB, Confidence: %.1f%%",
                                     i+1, signals[i].frequency, signals[i].snr_db, 
                                     signals[i].confidence * 100.0);
                        }
                        
                        last_signal_freq = signals[0].frequency + config.center_frequency;
                    }
                    
                    if (decoder && got > 0) {
                        if (num_signals > 0) {
                            cw_decoder_set_tone(decoder, signals[0].frequency);
                        }
                        cw_decoder_process_iq(decoder, iq_buffer, got);

                        decoded_callsign_t dec = cw_decoder_get_callsign(decoder);
                        if (dec.valid && dec.callsign[0] && last_signal_freq > 100.0f) {
                            LOG_INFO("SPOT: %s %.0f CW %+.1fdB", dec.callsign, last_signal_freq, 
                                     (num_signals>0 ? signals[0].snr_db : 0));
                            
                            spot_t rspot;
                            memset(&rspot, 0, sizeof(rspot));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
                            strncpy(rspot.callsign, dec.callsign, sizeof(rspot.callsign)-1);
#pragma GCC diagnostic pop
                            rspot.callsign[sizeof(rspot.callsign)-1] = '\0';
                            rspot.frequency_hz = last_signal_freq;
                            rspot.snr_db = num_signals > 0 ? signals[0].snr_db : 0;
                            rspot.confidence = dec.confidence;
                            rspot.timestamp = (unsigned long)time(NULL);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
                            strncpy(rspot.mode, "CW", sizeof(rspot.mode)-1);
#pragma GCC diagnostic pop
                            rspot.mode[sizeof(rspot.mode)-1] = '\0';
                            if (reporter) spot_reporter_submit_spot(reporter, &rspot);
                        }
                    }
                }
            }
        } else {
            /* Not connected: brief sleep to avoid busy loop, allow reconnect logic if any */
            usleep(100000);
        }
        
        /* occasional retry drain (use time based to not depend on reads) */
        if ((read_count + 1) % 200 == 0 && reporter) {
            spot_reporter_process_retries(reporter);
        }
        
        // Print stats periodically
        if (read_count % 100 == 0 && read_count > 0) {
            int available = tci_buffer_available(radio);
            LOG_DEBUG("Status: %d reads, %d samples processed, buffer: %d samples",
                      read_count, samples_processed, available);
        }
        
        usleep(10000);  // Sleep 10ms between reads
    }
    
    LOG_INFO("Shutting down...");
    LOG_INFO("Total: %d samples processed in %d read operations", samples_processed, read_count);
    
    free(iq_buffer);
    spot_reporter_destroy(reporter);
    cw_decoder_destroy(decoder);
    cw_decoder_global_shutdown();
    cw_detector_destroy(detector);
    tci_client_destroy(radio);
    logger_close();
    
    return 0;
}
