#ifndef FUSE_OPS_H
#define FUSE_OPS_H

#include "core_fs.h"

// Start the libfuse3 event loop.
// Passes the system context (gbfs_state_t) to the FUSE driver.
int run_fuse_fs(int argc, char *argv[], gbfs_state_t *state);

#endif // FUSE_OPS_H
