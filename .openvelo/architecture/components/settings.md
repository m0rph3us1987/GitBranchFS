# Settings component

The Settings component is responsible for loading, parsing, and managing application-wide user configuration options from the `~/.gitbranchfs/config.json` file. It relies on the `jansson` library to parse the JSON format. 

If the configuration file `~/.gitbranchfs/config.json` does not exist on startup (which typically indicates a first-time start), the settings layer automatically creates the directory `~/.gitbranchfs` and writes a default `config.json` file containing default settings.

## Key files

| File                 | Responsibility                                                                                            |
|----------------------|-----------------------------------------------------------------------------------------------------------|
| `include/settings.h` | Defines the `gbfs_settings_t` configuration structure and the public API for initializing, freeing, and loading application settings. |
| `src/settings.c`     | Implements the loading, default creation, and parsing lifecycle of settings using `jansson`.                 |

## Data Structures

```c
typedef struct {
    char *overlay_root;     // malloc'd, NULL means "use hardcoded default"
} gbfs_settings_t;
```

* `overlay_root`: Represents the base directory under which per-mountpoint overlay directories are created. Defaults to `NULL` (which falls back to `~/.gitbranchfs`).

## Public API

### `void gbfs_settings_init(gbfs_settings_t *s)`
Initializes the settings structure, setting all fields to their default state (`s->overlay_root = NULL`).

### `void gbfs_settings_free(gbfs_settings_t *s)`
Releases any heap resources owned by the configuration structure (i.e. `s->overlay_root`) and resets fields to NULL.

### `int gbfs_settings_load(gbfs_settings_t *s)`
Performs the core load logic:
1. Resolves the path to the configuration file using `resolve_home_path("~/.gitbranchfs/config.json")`.
2. Checks if the file exists:
   - If the file **does not exist**, it ensures the directory `~/.gitbranchfs` exists (using `mkdir_rec`) and creates a default `config.json` file containing:
     ```json
     {
       "overlay_path": "~/.gitbranchfs"
     }
     ```
     An informative message is printed to `stdout` notifying the user of the file creation.
3. Reads the file contents using `read_file_contents`.
4. Parses the file content via `json_loadb`.
   - If the JSON is malformed or invalid, warning messages are printed to `stderr` and the loader continues with defaults (returning 0).
5. Inspects the parsed JSON object for the optional `overlay_path` field:
   - Verifies the field is a non-empty string.
   - If valid, duplicates it into `s->overlay_root`.
   - If the field is invalid (e.g. not a string), logs a warning to `stderr` and continues.
6. Returns `0` on success or soft-failure (missing/invalid config, fallback to defaults), or `-1` on hard failure (e.g. out-of-memory).

## Configuration File Schema

```json
{
  "overlay_path": "~/.gitbranchfs"
}
```

* `overlay_path`: (Optional) Root directory used for per-mount overlay state. May be an absolute path or use `~` for the user's home directory.

## Error and Warning Logs

| Scenario | Behavior | Log Output |
|---|---|---|
| Config path resolution fails | Falls back to default. | `Warning: Could not resolve config file path; using defaults.` |
| File does not exist | Automatically creates default directory and `config.json`. | `Created default configuration file at: <resolved_path>` |
| Default file write failure | Continues using defaults. | `Warning: Could not create default config file at: <resolved_path>` |
| Malformed JSON | Falls back to defaults. | `Warning: Failed to parse <path> at line <l> column <c>: <err_text>. Using defaults.` |
| JSON root is not an object | Falls back to defaults. | `Warning: Settings file root is not a JSON object. Using defaults.` |
| `overlay_path` not a string | Falls back to defaults. | `Warning: 'overlay_path' must be a string. Using defaults.` |
