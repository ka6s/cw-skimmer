#include "cw_message_validator.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>

static const char *const k_keywords[] = {
    "CQ", "DE", "TEST", "TU", "73", "GN", "BK", "KN", "AR", "SK",
    "RBN", "QSL", "QRZ", "AGN", "PSE", "RST", "QTH", "RIG", "WX",
    "5NN", "NNN", "SRI", "CPY", "HW", "VY", "GB", "ES", "AS", "K",
    NULL
};

typedef struct {
    float display_min;
    float accept_min;
} validation_thresholds_t;

static const validation_thresholds_t k_thresholds[] = {
    {0.40f, 0.35f},  /* minimal: show more, still filter obvious noise */
    {0.55f, 0.50f},  /* normal: hide low-confidence decode clutter */
    {0.65f, 0.60f},  /* aggressive */
    {0.78f, 0.75f}   /* paranoid */
};

cw_validation_mode_t cw_message_validator_parse_mode(const char *mode_str)
{
    if (!mode_str || mode_str[0] == '\0') {
        return CW_VALIDATION_NORMAL;
    }
    if (strcasecmp(mode_str, "minimal") == 0) {
        return CW_VALIDATION_MINIMAL;
    }
    if (strcasecmp(mode_str, "aggressive") == 0) {
        return CW_VALIDATION_AGGRESSIVE;
    }
    if (strcasecmp(mode_str, "paranoid") == 0) {
        return CW_VALIDATION_PARANOID;
    }
    return CW_VALIDATION_NORMAL;
}

float cw_message_validator_display_threshold(cw_validation_mode_t mode)
{
    int idx = (int)mode;
    if (idx < 0 || idx > CW_VALIDATION_PARANOID) {
        idx = CW_VALIDATION_NORMAL;
    }
    return k_thresholds[idx].display_min;
}

float cw_message_validator_accept_threshold(cw_validation_mode_t mode)
{
    int idx = (int)mode;
    if (idx < 0 || idx > CW_VALIDATION_PARANOID) {
        idx = CW_VALIDATION_NORMAL;
    }
    return k_thresholds[idx].accept_min;
}

static int is_callsign_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '/' || c == ' ';
}

static int text_has_only_valid_chars(const char *text)
{
    int i;
    if (!text) {
        return 0;
    }
    for (i = 0; text[i] != '\0'; ++i) {
        if (!is_callsign_char(text[i])) {
            return 0;
        }
    }
    return text[0] != '\0';
}

static int is_word_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '/';
}

static int text_contains_keyword(const char *text)
{
    int i;
    size_t len;
    size_t kw_len;
    char upper[64];
    const char *pos;

    if (!text) {
        return 0;
    }

    len = strlen(text);
    if (len >= sizeof(upper)) {
        len = sizeof(upper) - 1;
    }
    for (i = 0; i < (int)len; ++i) {
        upper[i] = (char)toupper((unsigned char)text[i]);
    }
    upper[len] = '\0';

    for (i = 0; k_keywords[i] != NULL; ++i) {
        kw_len = strlen(k_keywords[i]);
        pos = upper;
        while ((pos = strstr(pos, k_keywords[i])) != NULL) {
            int start_is_boundary = (pos == upper) || !is_word_char(pos[-1]);
            int end_is_boundary = (pos[kw_len] == '\0') || !is_word_char(pos[kw_len]);
            if (start_is_boundary && end_is_boundary) {
                return 1;
            }
            pos += kw_len;
        }
    }
    return 0;
}

static int itu_prefix_plausible(const char *text)
{
    int i;
    int len;
    int has_digit = 0;
    int letter_count = 0;

    if (!text || text[0] == '\0') {
        return 0;
    }

    if (!isalpha((unsigned char)text[0])) {
        return 0;
    }

    len = (int)strlen(text);
    for (i = 0; i < len && i < 4; ++i) {
        if (isalpha((unsigned char)text[i])) {
            letter_count++;
        } else if (isdigit((unsigned char)text[i])) {
            has_digit = 1;
            break;
        } else if (text[i] == ' ') {
            break;
        } else {
            return 0;
        }
    }

    if (!has_digit) {
        return 0;
    }

    if (letter_count >= 1 && letter_count <= 2) {
        return 1;
    }
    return 0;
}

static int digit_group_count(const char *text)
{
    int i;
    int groups = 0;
    int in_digit = 0;

    if (!text) {
        return 0;
    }

    for (i = 0; text[i] != '\0'; ++i) {
        if (isdigit((unsigned char)text[i])) {
            if (!in_digit) {
                groups++;
                in_digit = 1;
            }
        } else {
            in_digit = 0;
        }
    }
    return groups;
}

static int suffix_letters_after_digit(const char *text)
{
    int i;
    int len;
    int seen_digit = 0;
    int suffix = 0;

    if (!text) {
        return 0;
    }

    len = (int)strlen(text);
    for (i = 0; i < len; ++i) {
        if (isdigit((unsigned char)text[i])) {
            seen_digit = 1;
            suffix = 0;
        } else if (seen_digit && isalpha((unsigned char)text[i])) {
            suffix++;
        } else if (text[i] == '/') {
            break;
        }
    }
    return suffix;
}

static int looks_like_callsign(const char *text)
{
    int len;
    int i;
    int alpha = 0;
    int digit = 0;

    if (!text) {
        return 0;
    }

    len = (int)strlen(text);
    if (len < 3 || len > 8) {
        return 0;
    }

    for (i = 0; i < len; ++i) {
        if (isalpha((unsigned char)text[i])) {
            alpha++;
        } else if (isdigit((unsigned char)text[i])) {
            digit++;
        } else if (text[i] != '/') {
            return 0;
        }
    }

    if (alpha < 2 || digit < 1 || !itu_prefix_plausible(text)) {
        return 0;
    }

    if (digit_group_count(text) != 1) {
        return 0;
    }

    {
        int suffix_len = suffix_letters_after_digit(text);
        int s;
        int has_vowel = 0;

        if (suffix_len < 1 || suffix_len > 3) {
            return 0;
        }

        if (suffix_len >= 3) {
            for (s = 0; text[s] != '\0'; ++s) {
                char c = (char)toupper((unsigned char)text[s]);
                if (c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U' || c == 'Y') {
                    has_vowel = 1;
                    break;
                }
            }
            if (!has_vowel) {
                return 0;
            }
        }
    }

    return 1;
}

static int max_run_length(const char *text)
{
    int max_run = 1;
    int run = 1;
    int i;

    if (!text || text[0] == '\0') {
        return 0;
    }

    for (i = 1; text[i] != '\0'; ++i) {
        if (text[i] == text[i - 1] && text[i] != ' ') {
            run++;
            if (run > max_run) {
                max_run = run;
            }
        } else {
            run = 1;
        }
    }
    return max_run;
}

static const char *primary_score_text(const char *text, char *word_buf, size_t buf_size)
{
    const char *end;
    const char *start;

    if (!text || text[0] == '\0') {
        return text;
    }

    end = text + strlen(text);
    while (end > text && end[-1] == ' ') {
        end--;
    }
    if (end == text) {
        return text;
    }

    start = end;
    while (start > text && start[-1] != ' ') {
        start--;
    }

    if (start < end) {
        size_t len = (size_t)(end - start);
        if (len >= buf_size) {
            len = buf_size - 1;
        }
        memcpy(word_buf, start, len);
        word_buf[len] = '\0';
        return word_buf;
    }

    return text;
}

static int text_has_digit(const char *text)
{
    int i;
    if (!text) {
        return 0;
    }
    for (i = 0; text[i] != '\0'; ++i) {
        if (isdigit((unsigned char)text[i])) {
            return 1;
        }
    }
    return 0;
}

static int digit_letter_chaos(const char *text)
{
    int transitions = 0;
    int prev_is_digit = -1;
    int i;

    if (!text) {
        return 0;
    }

    for (i = 0; text[i] != '\0'; ++i) {
        int is_digit;
        if (!isalpha((unsigned char)text[i]) && !isdigit((unsigned char)text[i])) {
            continue;
        }
        is_digit = isdigit((unsigned char)text[i]) ? 1 : 0;
        if (prev_is_digit >= 0 && is_digit != prev_is_digit) {
            transitions++;
        }
        prev_is_digit = is_digit;
    }

    return transitions >= 5;
}

static float clamp01(float v)
{
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

float cw_message_validator_score(const char *text, cw_validation_mode_t mode,
                                 const char *prior_word)
{
    char word_buf[64];
    const char *primary;
    int len;
    int primary_len;
    float score;

    (void)mode;

    if (!text || text[0] == '\0') {
        return 0.0f;
    }

    if (!text_has_only_valid_chars(text)) {
        return 0.05f;
    }

    primary = primary_score_text(text, word_buf, sizeof(word_buf));
    len = (int)strlen(text);
    primary_len = (int)strlen(primary);
    score = 0.18f;

    if (primary_len == 1) {
        return 0.22f;
    }

    if (text_contains_keyword(text)) {
        score += 0.24f;
    }
    if (text_contains_keyword(primary)) {
        score += 0.14f;
    }
    if (itu_prefix_plausible(primary)) {
        score += 0.18f;
    }
    if (looks_like_callsign(primary)) {
        score += 0.24f;
    }
    if (primary_len >= 3 && primary_len <= 7 && isalpha((unsigned char)primary[0])) {
        score += 0.06f;
    }

    if (prior_word && prior_word[0] != '\0' && strcmp(primary, prior_word) == 0) {
        score += 0.12f;
    }

    if (primary_len >= 2 && primary_len <= 4 &&
        !text_has_digit(primary) && !text_contains_keyword(primary)) {
        return clamp01(0.28f);
    }

    if (text_has_digit(primary) && primary_len >= 4 && primary_len <= 7 &&
        !looks_like_callsign(primary) && !text_contains_keyword(primary)) {
        score -= 0.28f;
    }

    if (suffix_letters_after_digit(primary) > 3) {
        score -= 0.18f;
    }

    if (len > 12) {
        score -= 0.25f;
    }
    if (max_run_length(primary) >= 4) {
        score -= 0.30f;
    }
    if (digit_letter_chaos(primary)) {
        score -= 0.25f;
    }
    if (primary_len >= 6 && !text_contains_keyword(primary) &&
        !looks_like_callsign(primary)) {
        score -= 0.22f;
    }

    return clamp01(score);
}

int cw_message_validator_accepts(const char *text, cw_validation_mode_t mode,
                                 const char *prior_word)
{
    float score = cw_message_validator_score(text, mode, prior_word);
    return score >= cw_message_validator_accept_threshold(mode);
}