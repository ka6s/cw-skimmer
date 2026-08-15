#include "bayesian_tree.h"
#include "logger.h"
#include <stdlib.h>
#include <math.h>

#define NUM_FEATURES 6

static const char *feature_names[NUM_FEATURES] = {
    "Tone Purity",
    "Keying Regularity", 
    "Signal-to-Noise Ratio",
    "Bandwidth",
    "Envelope Rise/Fall",
    "Adjacent Channel Rejection"
};

/**
 * Bayesian classifier weights trained for CW detection
 * These weights represent the importance of each feature in identifying CW signals
 */
static const float cw_weights[NUM_FEATURES] = {
    0.25,  // Tone purity - very important for CW (pure sine)
    0.20,  // Keying regularity - CW has consistent on/off timing
    0.15,  // SNR - helps distinguish signal from noise
    0.15,  // Bandwidth - CW is narrow (typically 50-200 Hz)
    0.15,  // Envelope shape - CW has defined keying envelope
    0.10   // Adjacent channel rejection - helps with filter selectivity
};

bayesian_classifier_t *bayesian_create(int num_features) {
    bayesian_classifier_t *bc = malloc(sizeof(bayesian_classifier_t));
    if (!bc) return NULL;
    
    bc->weights = malloc(num_features * sizeof(float));
    if (!bc->weights) {
        free(bc);
        return NULL;
    }
    
    // Copy pre-trained weights
    for (int i = 0; i < num_features && i < NUM_FEATURES; i++) {
        bc->weights[i] = cw_weights[i];
    }
    
    bc->num_features = num_features;
    bc->confidence_threshold = 0.60;  // 60% confidence threshold
    
    LOG_DEBUG("Bayesian classifier created with %d features", num_features);
    return bc;
}

float bayesian_evaluate(bayesian_classifier_t *classifier, const float *features) {
    if (!classifier || !features) return 0.0;
    
    // Bayesian probability calculation:
    // P(CW|features) = prod(P(feature_i|CW)) / prod(P(feature_i))
    // Simplified: weighted sum with sigmoid normalization
    
    float weighted_sum = 0.0;
    
    for (int i = 0; i < classifier->num_features && i < NUM_FEATURES; i++) {
        // Normalize feature to [0, 1]
        float feature_prob = fmaxf(0.0, fminf(1.0, features[i]));
        
        // Weight by feature importance
        weighted_sum += classifier->weights[i] * feature_prob;
    }
    
    // Apply sigmoid function for probability output
    // sigmoid(x) = 1 / (1 + e^(-x))
    float probability = 1.0 / (1.0 + expf(-5.0 * (weighted_sum - 0.5)));
    
    return probability;
}

const char **bayesian_get_feature_names(void) {
    return (const char **)feature_names;
}

void bayesian_destroy(bayesian_classifier_t *classifier) {
    if (!classifier) return;
    if (classifier->weights) free(classifier->weights);
    free(classifier);
    LOG_DEBUG("Bayesian classifier destroyed");
}
