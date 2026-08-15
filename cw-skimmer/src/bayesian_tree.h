#ifndef BAYESIAN_TREE_H
#define BAYESIAN_TREE_H

typedef struct {
    float threshold;
    float weight;
    const char *description;
} feature_t;

typedef struct {
    float *weights;
    int num_features;
    float confidence_threshold;
} bayesian_classifier_t;

/**
 * Create Bayesian classifier for CW detection
 * @param num_features Number of features to evaluate
 * @return Initialized classifier
 */
bayesian_classifier_t *bayesian_create(int num_features);

/**
 * Evaluate CW signal probability using Bayesian tree
 * Features: tone_purity (0-100), keying_regularity (0-100), snr_db (-20 to 60),
 *           bandwidth_hz (0-500), envelope_ratio (0-1), adjacent_channel_rejection (0-100)
 * @param classifier Bayesian classifier
 * @param features Array of feature values [0..1] normalized
 * @return Probability of CW signal (0.0 to 1.0)
 */
float bayesian_evaluate(bayesian_classifier_t *classifier, const float *features);

/**
 * Get feature names
 */
const char **bayesian_get_feature_names(void);

/**
 * Destroy classifier
 */
void bayesian_destroy(bayesian_classifier_t *classifier);

#endif
