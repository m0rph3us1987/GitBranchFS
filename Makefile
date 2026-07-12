CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_FILE_OFFSET_BITS=64 \
         -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
         $(shell pkg-config --cflags libgit2 fuse3 jansson)
LDFLAGS = -static
LIBS = $(shell pkg-config --static --libs libgit2 fuse3 jansson) -lssl -lcrypto -lpthread -ldl

TARGET = gbfs
SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
INCLUDES = -Iinclude

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Installed $(TARGET) to $(DESTDIR)$(BINDIR)/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Removed $(DESTDIR)$(BINDIR)/$(TARGET)"

clean:
	rm -f src/*.o $(TARGET)
