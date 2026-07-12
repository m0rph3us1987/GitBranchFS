#define _GNU_SOURCE
#include "compat.h"
#include "settings.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

void gbfs_settings_init(gbfs_settings_t *s) {
    if (s == NULL) return;
    s->overlay_root = NULL;
}

void gbfs_settings_free(gbfs_settings_t *s) {
    if (s == NULL) return;
    free(s->overlay_root);
    s->overlay_root = NULL;
}

static char *read_file_contents(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    if (out_len != NULL) *out_len = (size_t)size;
    return buf;
}

int gbfs_settings_load(gbfs_settings_t *s) {
    if (s == NULL) return -1;
    gbfs_settings_init(s);

    char *config_path = resolve_home_path("~/.gitbranchfs/config.json");
    if (config_path == NULL) {
        fprintf(stderr, "Warning: Could not resolve config file path; using defaults.\n");
        return 0;
    }

    if (!file_exists(config_path)) {
        char *config_dir = resolve_home_path("~/.gitbranchfs");
        if (config_dir != NULL) {
            mkdir_rec(config_dir);
            free(config_dir);
        }
        FILE *f = fopen(config_path, "w");
        if (f != NULL) {
            fprintf(f, "{\n  \"overlay_path\": \"~/.gitbranchfs\"\n}\n");
            fclose(f);
            printf("Created default configuration file at: %s\n", config_path);
        } else {
            fprintf(stderr, "Warning: Could not create default config file at: %s\n", config_path);
        }
    }

    size_t len = 0;
    char *contents = read_file_contents(config_path, &len);
    if (contents == NULL) {
        free(config_path);
        return 0;
    }

    json_error_t err;
    json_t *root = json_loadb(contents, len, 0, &err);
    free(contents);
    if (root == NULL) {
        fprintf(stderr,
                "Warning: Failed to parse %s at line %d column %d: %s. Using defaults.\n",
                config_path, err.line, err.column, err.text);
        free(config_path);
        return 0;
    }
    free(config_path);

    if (!json_is_object(root)) {
        fprintf(stderr, "Warning: Settings file root is not a JSON object. Using defaults.\n");
        json_decref(root);
        return 0;
    }

    json_t *overlay = json_object_get(root, "overlay_path");
    if (overlay != NULL && !json_is_null(overlay)) {
        if (!json_is_string(overlay)) {
            fprintf(stderr, "Warning: 'overlay_path' must be a string. Using defaults.\n");
        } else {
            const char *v = json_string_value(overlay);
            if (v != NULL && v[0] != '\0') {
                s->overlay_root = strdup(v);
                if (s->overlay_root == NULL) {
                    json_decref(root);
                    return -1;
                }
            }
        }
    }

    json_decref(root);
    return 0;
}