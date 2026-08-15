#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "config.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

void config_defaults(config_t *config) {
    strcpy(config->radio_host, "192.168.2.146");
    config->radio_port = 50001;
    strcpy(config->radio_protocol, "websocket");
    config->center_frequency = 14074000;
    config->sample_rate = 48000;
    
    strcpy(config->spot_server_host, "192.168.2.146");
    config->spot_server_port = 7373;
    strcpy(config->spot_server_callsign, "CWSKIMMER");
    
    config->detection_threshold = 14;
    config->min_snr_db = 2.0f;
    config->decode_min_snr_db = 5.0f;
    config->decode_bucket_hz = 1000.0f;
    strcpy(config->validation_mode, "normal");
    config->spot_enabled = 0;
    config->log_level = 1;  // INFO
    strcpy(config->log_file, "cw-skimmer.log");
    strcpy(config->deepcw_model_path, "models/model.onnx");
    config->spectrum_span_hz = 0;  /* 0 = full-band wide (production default) */
    strcpy(config->tci_stream_mode, "iq");  /* iq | audio */
    config->multi_decode_channels = 1;
}

static char *trim_string(char *str) {
    while (*str && (*str == ' ' || *str == '\t')) str++;
    int len = strlen(str);
    while (len > 0 && (str[len-1] == ' ' || str[len-1] == '\t' || str[len-1] == '\n')) {
        str[--len] = '\0';
    }
    return str;
}

static int config_file_exists(const char *path) {
    return access(path, R_OK) == 0;
}

static int config_path_is_absolute(const char *path) {
    return path && path[0] == '/';
}

static void config_dirname(const char *path, char *out, size_t out_size) {
    if (!path || !path[0]) {
        snprintf(out, out_size, ".");
        return;
    }

    snprintf(out, out_size, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash) {
        if (slash == out) {
            out[1] = '\0';
        } else {
            *slash = '\0';
        }
    } else {
        snprintf(out, out_size, ".");
    }
}

static int config_get_exe_dir(char *out, size_t out_size) {
    char exe[PATH_MAX];
    ssize_t exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    char *slash;

    if (exe_len <= 0) {
        return -1;
    }

    exe[exe_len] = '\0';
    slash = strrchr(exe, '/');
    if (!slash) {
        return -1;
    }

    *slash = '\0';
    snprintf(out, out_size, "%s", exe);
    return 0;
}

static int config_try_resolve_path(const char *base_dir, const char *rel_path,
                                   char *out, size_t out_size) {
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];
    char base_resolved[PATH_MAX];
    const char *base = base_dir;

    if (!base_dir || !rel_path || !rel_path[0]) {
        return -1;
    }

    if (realpath(base_dir, base_resolved)) {
        base = base_resolved;
    }

    if (snprintf(candidate, sizeof(candidate), "%s/%s", base, rel_path) >= (int)sizeof(candidate)) {
        return -1;
    }

    if (realpath(candidate, resolved) && access(resolved, R_OK) == 0) {
        snprintf(out, out_size, "%s", resolved);
        return 0;
    }

    return -1;
}

static void config_resolve_deepcw_model(config_t *config, const char *config_path) {
    char resolved[PATH_MAX];
    char base_dir[PATH_MAX];
    char exe_dir[PATH_MAX];
    const char *rel = config->deepcw_model_path;

    if (!rel || !rel[0] || config_path_is_absolute(rel)) {
        return;
    }

    if (config_path && config_path[0]) {
        config_dirname(config_path, base_dir, sizeof(base_dir));
        if (config_try_resolve_path(base_dir, rel, resolved, sizeof(resolved)) == 0) {
            snprintf(config->deepcw_model_path, sizeof(config->deepcw_model_path), "%s", resolved);
            LOG_INFO("Resolved DeepCW model path: %s", config->deepcw_model_path);
            return;
        }
    }

    if (config_get_exe_dir(exe_dir, sizeof(exe_dir)) == 0) {
        if (config_try_resolve_path(exe_dir, rel, resolved, sizeof(resolved)) == 0) {
            snprintf(config->deepcw_model_path, sizeof(config->deepcw_model_path), "%s", resolved);
            LOG_INFO("Resolved DeepCW model path: %s", config->deepcw_model_path);
            return;
        }

        snprintf(base_dir, sizeof(base_dir), "%s/..", exe_dir);
        if (config_try_resolve_path(base_dir, rel, resolved, sizeof(resolved)) == 0) {
            snprintf(config->deepcw_model_path, sizeof(config->deepcw_model_path), "%s", resolved);
            LOG_INFO("Resolved DeepCW model path: %s", config->deepcw_model_path);
            return;
        }
    }

    LOG_WARN("DeepCW model not found at configured path: %s", rel);
}

int config_load(const char *path, config_t *config) {
    config_defaults(config);

    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == ';') continue;
        
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *key = trim_string(line);
        char *value = trim_string(eq + 1);
        
        if (strcmp(key, "radio_host") == 0) strcpy(config->radio_host, value);
        else if (strcmp(key, "radio_port") == 0) config->radio_port = atoi(value);
        else if (strcmp(key, "radio_protocol") == 0) strcpy(config->radio_protocol, value);
        else if (strcmp(key, "center_frequency") == 0) config->center_frequency = atof(value);
        else if (strcmp(key, "sample_rate") == 0) config->sample_rate = atoi(value);
        else if (strcmp(key, "spot_server_host") == 0) strcpy(config->spot_server_host, value);
        else if (strcmp(key, "spot_server_port") == 0) config->spot_server_port = atoi(value);
        else if (strcmp(key, "spot_server_callsign") == 0) strcpy(config->spot_server_callsign, value);
        else if (strcmp(key, "detection_threshold") == 0) config->detection_threshold = atoi(value);
        else if (strcmp(key, "min_snr_db") == 0) config->min_snr_db = (float)atof(value);
        else if (strcmp(key, "decode_min_snr_db") == 0) config->decode_min_snr_db = (float)atof(value);
        else if (strcmp(key, "decode_bucket_hz") == 0) config->decode_bucket_hz = (float)atof(value);
        else if (strcmp(key, "validation_mode") == 0) strcpy(config->validation_mode, value);
        else if (strcmp(key, "spot_enabled") == 0) config->spot_enabled = atoi(value) ? 1 : 0;
        else if (strcmp(key, "log_level") == 0) config->log_level = atoi(value);
        else if (strcmp(key, "log_file") == 0) strcpy(config->log_file, value);
        else if (strcmp(key, "deepcw_model_path") == 0) strcpy(config->deepcw_model_path, value);
        else if (strcmp(key, "spectrum_span_hz") == 0) config->spectrum_span_hz = atoi(value);
        else if (strcmp(key, "tci_stream_mode") == 0) {
            strncpy(config->tci_stream_mode, value, sizeof(config->tci_stream_mode) - 1);
            config->tci_stream_mode[sizeof(config->tci_stream_mode) - 1] = '\0';
        }
        else if (strcmp(key, "multi_decode_channels") == 0) {
            int n = atoi(value);
            if (n < 1) n = 1;
            if (n > 16) n = 16;
            config->multi_decode_channels = n;
        }
    }
    
    fclose(f);
    LOG_INFO("Configuration loaded from %s", path);
    config_resolve_deepcw_model(config, path);
    return 0;
}

int config_load_auto(const char *explicit_path, config_t *config) {
    char exe[PATH_MAX];
    char path_exe[PATH_MAX];
    char path_parent[PATH_MAX];
    ssize_t exe_len;
    const char *candidates[8];
    int count = 0;

    config_defaults(config);

    if (explicit_path && explicit_path[0]) {
        candidates[count++] = explicit_path;
    }
    candidates[count++] = "cw-skimmer.conf";

    exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (exe_len > 0) {
        char *slash;

        exe[exe_len] = '\0';
        slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(path_exe, sizeof(path_exe), "%s/cw-skimmer.conf", exe);
            candidates[count++] = path_exe;
            snprintf(path_parent, sizeof(path_parent), "%s/../cw-skimmer.conf", exe);
            candidates[count++] = path_parent;
        }
    }

    for (int i = 0; i < count; i++) {
        if (config_file_exists(candidates[i]) && config_load(candidates[i], config) == 0) {
            return 0;
        }
    }

    LOG_WARN("Config file not found, using defaults");
    config_resolve_deepcw_model(config, NULL);
    return -1;
}
