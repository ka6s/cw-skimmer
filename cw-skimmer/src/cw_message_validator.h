#ifndef CW_MESSAGE_VALIDATOR_H
#define CW_MESSAGE_VALIDATOR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CW_VALIDATION_MINIMAL = 0,
    CW_VALIDATION_NORMAL = 1,
    CW_VALIDATION_AGGRESSIVE = 2,
    CW_VALIDATION_PARANOID = 3
} cw_validation_mode_t;

/**
 * Parse validation_mode from config string (minimal/normal/aggressive/paranoid).
 * Returns CW_VALIDATION_NORMAL for unknown values.
 */
cw_validation_mode_t cw_message_validator_parse_mode(const char *mode_str);

/**
 * Score decoded text 0.0–1.0 using ITU prefix, keyword, and structure heuristics.
 * @param prior_word Previous completed word on the same channel (NULL if none).
 */
float cw_message_validator_score(const char *text, cw_validation_mode_t mode,
                                 const char *prior_word);

/**
 * Confidence threshold for high-confidence (white) display at this mode.
 */
float cw_message_validator_display_threshold(cw_validation_mode_t mode);

/**
 * Minimum score to treat a completed word as a valid callsign/message.
 */
float cw_message_validator_accept_threshold(cw_validation_mode_t mode);

/**
 * Return 1 if text meets accept threshold for the given mode.
 */
int cw_message_validator_accepts(const char *text, cw_validation_mode_t mode,
                                 const char *prior_word);

#ifdef __cplusplus
}
#endif

#endif