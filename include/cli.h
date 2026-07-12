#ifndef CLI_H
#define CLI_H

// Execute the command-line command (mount, unmount, etc.).
// Returns 0 on success, non-zero on failure.
int cli_run(int argc, char *argv[]);

#endif // CLI_H
