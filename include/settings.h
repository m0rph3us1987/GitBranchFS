#ifndef SETTINGS_H
#define SETTINGS_H

// User-configurable application settings loaded from
// ~/.gitbranchfs/config.json. Missing file or missing fields are
// not errors: fields default to the historical hardcoded values.

typedef struct {
    char *overlay_root;
} gbfs_settings_t;

// Initialize an empty settings struct (all fields NULL/default).
void gbfs_settings_init(gbfs_settings_t *s);

// Release any resources owned by a settings struct.
void gbfs_settings_free(gbfs_settings_t *s);

// Load settings from ~/.gitbranchfs/config.json. A missing file is
// not an error; the returned struct simply has overlay_root == NULL.
// Malformed JSON or a wrongly-typed overlay_path field is logged to
// stderr and treated as if the field were absent.
// Returns 0 on success or soft-failure, -1 on hard failure (e.g. OOM).
int gbfs_settings_load(gbfs_settings_t *s);

#endif // SETTINGS_H