# Build system

The build is a handwritten GNU Makefile (24 lines) with pkg-config
guards for libgit2 and libfuse3, automatic source discovery via
`wildcard`, and a single non-test target set (`all`, `clean`). Add a
new `.c` file anywhere under `src/` and the Makefile picks it up —
no edits required.

## Key files

| File         | Responsibility                                                       |
|--------------|----------------------------------------------------------------------|
| `Makefile`   | All targets and the only build entry point.                          |

There is no `build/` directory, no per-TU compile flags, no
`tests/` directory, no `make test`.

## Targets

- `all` (default) — compiles every `src/*.c` and links `./gbfs`.
- `clean` — removes every `src/*.o` and `./gbfs`.

The build is fully parallel-safe — there are no shared side-effects
between `$(OBJS)` and no test binaries with `O_EXCL` filesystem state.

## Source discovery

```make
SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
```

Adding a new source file is a `touch src/newfile.c && make` — no
Makefile edits needed.

## Compiler / linker baseline

```make
CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_FILE_OFFSET_BITS=64 \
         -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
         $(shell pkg-config --cflags libgit2 fuse3 jansson)
LDFLAGS = -static
LIBS = $(shell pkg-config --static --libs libgit2 fuse3 jansson) -lssl -lcrypto -lpthread -ldl
INCLUDES = -Iinclude
```

- `-Wall -Wextra -O2 -std=c11` — strict-ish C11 with the usual warning surface.
- `-D_FILE_OFFSET_BITS=64` — needed for the `off_t` / `pwrite` / `pread` calls inside `core_fs.c`.
- `-fstack-protector-strong` and `-D_FORTIFY_SOURCE=2` — security hardening flags.
- `-static` in `LDFLAGS` — instructs the linker to produce a fully statically linked binary so it can easily be shipped inside a docker image (e.g. scratch, alpine).
- `pkg-config --static --libs libgit2 fuse3 jansson` — provides the static libraries and dependencies paths.
- `-lssl -lcrypto -lpthread -ldl` — OpenSSL, threading, and dynamic loading dependencies linked statically.
- `src/gssapi_stubs.c` — a stub library automatically compiled to satisfy GSSAPI symbols requested by `libgit2.a` without requiring host static Kerberos libraries.

## Targets in detail

```make
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
        rm -f src/*.o $(TARGET)
```

The `%.o: %.c` pattern rule places the object file next to its
source (`src/foo.c` → `src/foo.o`), which is why `clean` glob-removes
`src/*.o`. There is no separate `build/` artefact tree; the binary is
left at the project root.

## pkg-config guards

The Makefile does **not** wrap `pkg-config` in a conditional or
fallback shell block — if either `libgit2` or `fuse3` is missing,
`pkg-config` itself prints the error and `$(CFLAGS)`/`$(LIBS)`
become whatever the failed `pkg-config` writes (usually a non-zero
exit that aborts the build). The expected workflow when the
diagnostic appears is the standard Debian/Ubuntu one-liner:

```
sudo apt install build-essential pkg-config libgit2-dev libfuse3-dev
```

Equivalent one-liners for Fedora / Arch are documented upstream in
`libgit2` and `fuse3` package documentation; we do not shell out to
detect distro. A future enhancement could add an
`apt`/`dnf`/`pacman`-aware helper script, but today this lives only in
the README or installation docs.

## When you should edit the Makefile

Almost never:

- **Adding a new source file** — drop it under `src/` and run
  `make`. Auto-discovery handles the rest.
- **Adding a new header** — drop it under `include/` and `#include`
  it from your `.c` files. `-Iinclude` makes it reachable.
- **Adding a new external library** — add its `pkg-config` flags to
  the `CFLAGS` and `LIBS` lines. Update this doc.
- **Adding a new build mode** (e.g., release with `-O3`/`-g`) —
  edit the `CFLAGS` line.
- **Adding a test framework** — today there is no `tests/`
  directory. A future enhancement that introduces one would need a
  new `test` target, a `TEST_SRCS = $(wildcard tests/*.c)` line,
  per-test link rules, and updates to this doc.

Anything else — a new component in `src/`, a new gbfs operation in
`core_fs.c`, a new libgit2 wrapper helper, a new overlay state file —
is purely C-level work and does not touch the Makefile.
