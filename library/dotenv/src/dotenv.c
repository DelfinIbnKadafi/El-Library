
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include "elplugin.h"

#define ENV_MAX_SIZE 4096
#define ENV_MAX_VARS 128
#define ENV_KEY_SIZE 128
#define ENV_VALUE_SIZE 512

static const ELApi *api;

typedef struct {
    char key[ENV_KEY_SIZE];
    char value[ENV_VALUE_SIZE];
} EnvEntry;

static EnvEntry env_entries[ENV_MAX_VARS];
static int env_count = 0;

// Remove whitespace from both ends.
static char *trim(char *str) {
    while (isspace((unsigned char)*str)) str++;

    char *end = str + strlen(str);

    while (end > str && isspace((unsigned char)*(end - 1))) {
        end--;
    }

    *end = '\0';
    return str;
}

// Parse the .env file.
static int read_env(void) {
    FILE *file = fopen(".env", "r");

    if (file == NULL) {
        fprintf(stderr, "[dotenv] Could not open .env\n");
        return 0;
    }

    char contents[ENV_MAX_SIZE];

    size_t length = fread(contents, 1, sizeof(contents) - 1, file);

    if (ferror(file)) {
        fprintf(stderr, "[dotenv] Error reading .env\n");
        fclose(file);
        return 0;
    }

    // Detect files that exceed the buffer.
    if (length == sizeof(contents) - 1 && !feof(file)) {
        fprintf(stderr, "[dotenv] .env file is too large\n");
        fclose(file);
        return 0;
    }

    contents[length] = '\0';
    fclose(file);

    char *line = contents;

    while (*line) {
        char *next = strchr(line, '\n');

        if (next != NULL) {
            *next = '\0';
        }

        char *current = trim(line);

        // Skip empty lines and comments.
        if (*current && *current != '#') {
            char *separator = strchr(current, '=');

            if (separator != NULL && env_count < ENV_MAX_VARS) {
                *separator = '\0';

                char *key = trim(current);
                char *value = trim(separator + 1);

                size_t value_len = strlen(value);

                // Remove surrounding quotes.
                if (value_len >= 2 &&
                    ((value[0] == '"' && value[value_len - 1] == '"') ||
                     (value[0] == '\'' && value[value_len - 1] == '\''))) {
                    value[value_len - 1] = '\0';
                    value++;
                }

                if (*key &&
                    strlen(key) < ENV_KEY_SIZE &&
                    strlen(value) < ENV_VALUE_SIZE) {

                    EnvEntry *entry = &env_entries[env_count];

                    strcpy(entry->key, key);
                    strcpy(entry->value, value);

                    env_count++;
                }
            }
        }

        if (next == NULL) break;
        line = next + 1;
    }

    return 1;
}

// Find environment variable.
static const char *find_env(const char *key) {
    for (int i = 0; i < env_count; i++) {
        if (strcmp(env_entries[i].key, key) == 0) {
            return env_entries[i].value;
        }
    }

    return NULL;
}

// Validate integer.
static int parse_int(const char *value, long long *result) {
    char *endptr;

    errno = 0;
    long long number = strtoll(value, &endptr, 10);

    if (value == endptr || *trim(endptr) != '\0' || errno == ERANGE) {
        return 0;
    }

    if (number < INT_MIN || number > INT_MAX) {
        return 0;
    }

    *result = number;
    return 1;
}

// Validate float.
static int parse_float(const char *value, double *result) {
    char *endptr;

    errno = 0;
    double number = strtod(value, &endptr);

    if (value == endptr || *trim(endptr) != '\0' ||
        errno == ERANGE || !isfinite(number)) {
        return 0;
    }

    *result = number;
    return 1;
}

// env_int(key)
static void get_env_int(ELCall *call) {
    const char *key = api->GetStr(call, 0);
    const char *value = find_env(key);

    if (value == NULL) {
        api->ReturnNone(call);
        return;
    }

    long long number;

    if (!parse_int(value, &number)) {
        api->Error(call, "Invalid integer value for environment key '%s'", key);
        return;
    }

    api->ReturnInt(call, number);
}

// env_float(key)
static void get_env_float(ELCall *call) {
    const char *key = api->GetStr(call, 0);
    const char *value = find_env(key);

    if (value == NULL) {
        api->ReturnNone(call);
        return;
    }

    double number;

    if (!parse_float(value, &number)) {
        api->Error(call, "Invalid float value for environment key '%s'", key);
        return;
    }

    api->ReturnFloat(call, number);
}

// env_bool(key)
static void get_env_bool(ELCall *call) {
    const char *key = api->GetStr(call, 0);
    const char *value = find_env(key);

    if (value == NULL) {
        api->ReturnNone(call);
        return;
    }

    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        api->ReturnBool(call, 1);
    } else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        api->ReturnBool(call, 0);
    } else {
        api->Error(call, "Invalid boolean value for environment key '%s'", key);
        return;
    }
}

// env_str(key)
static void get_env_str(ELCall *call) {
    const char *key = api->GetStr(call, 0);
    const char *value = find_env(key);

    if (value == NULL) {
        api->ReturnNone(call);
        return;
    }

    api->ReturnStr(call, value);
}

// Plugin API version.
EL_EXPORT int elp_Supports(void) {
    return EL_PLUGIN_API_VERSION;
}

// Load plugin.
EL_EXPORT int elp_Load(const ELApi *a) {
    if (a == NULL) return 1;

    api = a;

    if (!read_env()) {
        return 1;
    }

    if (!api->Register("env_int(str):int", get_env_int)) return 1;
    if (!api->Register("env_float(str):float", get_env_float)) return 1;
    if (!api->Register("env_bool(str):bool", get_env_bool)) return 1;
    if (!api->Register("env_str(str):str", get_env_str)) return 1;

    return 0;
}

// Unload plugin.
EL_EXPORT void elp_Unload(void) {
    memset(env_entries, 0, sizeof(env_entries));
    env_count = 0;
    api = NULL;
}
