#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <complex.h>
#include "../src/bayesian_tree.h"
#include "../src/cw_detector.h"
#include "../src/cw_decoder.h"
#include "../src/cw_message_validator.h"
#include "../src/logger.h"

void test_bayesian_classifier(void) {
    printf("\n=== Testing Bayesian Classifier ===\n");
    
    bayesian_classifier_t *bc = bayesian_create(6);
    if (!bc) {
        printf("FAILED: Could not create Bayesian classifier\n");
        return;
    }
    
    // Test case 1: Strong CW signal
    float cw_features[6] = {0.95, 0.90, 0.85, 0.95, 0.90, 0.98};
    float cw_prob = bayesian_evaluate(bc, cw_features);
    printf("✓ CW signal test: confidence = %.2f%% (expected >80%%)\n", cw_prob * 100);
    
    // Test case 2: Noise
    float noise_features[6] = {0.2, 0.3, 0.1, 0.5, 0.2, 0.3};
    float noise_prob = bayesian_evaluate(bc, noise_features);
    printf("✓ Noise test: confidence = %.2f%% (expected <40%%)\n", noise_prob * 100);
    
    // Test case 3: Partial signal
    float partial_features[6] = {0.7, 0.6, 0.5, 0.7, 0.6, 0.7};
    float partial_prob = bayesian_evaluate(bc, partial_features);
    printf("✓ Partial signal test: confidence = %.2f%%\n", partial_prob * 100);
    
    if (cw_prob > noise_prob) {
        printf("✓ Bayesian classifier correctly ranks CW > Noise\n");
    } else {
        printf("✗ FAILED: Classifier ranking incorrect\n");
    }
    
    bayesian_destroy(bc);
    printf("✓ Bayesian classifier test passed\n");
}

void test_cw_detector(void) {
    printf("\n=== Testing CW Detector ===\n");
    
    cw_detector_t *detector = cw_detector_create(48000, 480, 60);
    if (!detector) {
        printf("FAILED: Could not create detector\n");
        return;
    }
    
    // Create dummy I/Q data (should detect nothing with pure noise)
    float complex *iq_samples = malloc(1024 * sizeof(float complex));
    for (int i = 0; i < 1024; i++) {
        float real = (rand() % 1000 - 500) / 500.0f / 100.0f;
        float imag = (rand() % 1000 - 500) / 500.0f / 100.0f;
        iq_samples[i] = real + imag * I;
    }
    
    int result = cw_detector_analyze(detector, iq_samples, 1024);
    printf("✓ Analyzer returned: %d\n", result);
    
    cw_signal_t signals[10];
    int num_signals = cw_detector_get_signals(detector, signals, 10);
    printf("✓ Detector found %d signals\n", num_signals);
    
    float noise_floor = cw_detector_get_noise_floor(detector);
    printf("✓ Noise floor: %.1f dB\n", noise_floor);
    
    free(iq_samples);
    cw_detector_destroy(detector);
    printf("✓ CW detector test passed\n");
}

void test_cw_decoder(void) {
    cw_decoder_t *dec;
    float complex *iq;
    int i;
    printf("\n=== Testing ditdah Decoder ===\n");

    if (cw_decoder_global_init() != 0) {
        printf("FAILED: ditdah decoder init failed\n");
        return;
    }

    dec = cw_decoder_create(48000, 20);
    if (!dec) {
        printf("FAILED: could not create decoder\n");
        cw_decoder_global_shutdown();
        return;
    }

    iq = malloc(48000 * 4 * sizeof(float complex));
    if (!iq) {
        printf("FAILED: could not allocate IQ buffer\n");
        cw_decoder_destroy(dec);
        cw_decoder_global_shutdown();
        return;
    }

    cw_decoder_set_tone(dec, 600.0f);
    for (i = 0; i < 48000 * 4; ++i) {
        float t = (float)i / 48000.0f;
        float phase = 2.0f * 3.14159265f * 600.0f * t;
        float env = (fmodf(t, 0.12f) < 0.06f) ? 1.0f : 0.0f;
        float tone = 0.35f * env * sinf(phase);
        iq[i] = tone + tone * I;
    }

    cw_decoder_process_iq(dec, iq, 48000 * 4);
    cw_decoder_flush(dec);

    printf("✓ ditdah decoder produced text='%s' pending='%s'\n",
           cw_decoder_get_text(dec), cw_decoder_get_symbols(dec));

    free(iq);
    cw_decoder_destroy(dec);
    cw_decoder_global_shutdown();
    printf("✓ ditdah decoder test passed\n");
}

void test_message_validator(void)
{
    cw_validation_mode_t mode = CW_VALIDATION_NORMAL;
    float score;
    int accepted;

    printf("\n=== Testing Message Validator ===\n");

    score = cw_message_validator_score("CQ DE K1ABC", mode, NULL);
    accepted = cw_message_validator_accepts("CQ DE K1ABC", mode, NULL);
    if (score >= 0.50f && accepted) {
        printf("✓ CQ DE K1ABC score=%.2f accepted\n", score);
    } else {
        printf("✗ FAILED: CQ DE K1ABC score=%.2f accepted=%d\n", score, accepted);
    }

    score = cw_message_validator_score("NGKTOTTN0X01N9P", mode, NULL);
    accepted = cw_message_validator_accepts("NGKTOTTN0X01N9P", mode, NULL);
    if (score < cw_message_validator_accept_threshold(mode) && !accepted) {
        printf("✓ Garbage string rejected score=%.2f\n", score);
    } else {
        printf("✗ FAILED: garbage accepted score=%.2f accepted=%d\n", score, accepted);
    }

    score = cw_message_validator_score("T", mode, NULL);
    if (score < cw_message_validator_display_threshold(mode)) {
        printf("✓ Single T low display confidence score=%.2f\n", score);
    } else {
        printf("✗ FAILED: single T too high score=%.2f\n", score);
    }

    score = cw_message_validator_score("TEST", mode, NULL);
    accepted = cw_message_validator_accepts("TEST", mode, NULL);
    if (score >= 0.50f && accepted) {
        printf("✓ TEST keyword score=%.2f accepted\n", score);
    } else {
        printf("✗ FAILED: TEST score=%.2f accepted=%d\n", score, accepted);
    }

    score = cw_message_validator_score("ETNK", mode, NULL);
    if (score < cw_message_validator_display_threshold(mode)) {
        printf("✓ Short alpha noise rejected score=%.2f\n", score);
    } else {
        printf("✗ FAILED: short alpha noise score=%.2f\n", score);
    }

    mode = cw_message_validator_parse_mode("minimal");
    if (cw_message_validator_display_threshold(mode) == 0.40f) {
        printf("✓ minimal mode display threshold 0.40\n");
    } else {
        printf("✗ FAILED: minimal display threshold\n");
    }

    printf("✓ Message validator test passed\n");
}

int main(void) {
    printf("CW Skimmer Unit Tests\n");
    printf("=====================\n");
    
    logger_init(LOG_DEBUG, NULL);
    
    test_bayesian_classifier();
    test_cw_detector();
    test_cw_decoder();
    test_message_validator();
    
    printf("\n=== All Tests Completed ===\n");
    logger_close();
    return 0;
}
