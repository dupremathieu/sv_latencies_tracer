# SPDX-License-Identifier: Apache-2.0

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE
CFLAGS  += -Iinclude -Isrc
LDFLAGS ?=
LDLIBS  ?= -lpthread

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

BUILDDIR := build

# Source files per binary
COMMON_SRCS := src/sv_parser.c src/histogram.c src/drop_detector.c \
               src/frame_capture.c src/metrics.c src/system_monitor.c \
               src/config.c src/protocol.c
COMMON_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(COMMON_SRCS))

SUBSCRIBER_SRCS := src/main_subscriber.c
SUBSCRIBER_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SUBSCRIBER_SRCS))

AGENT_SRCS := src/main_capture_agent.c
AGENT_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(AGENT_SRCS))

COLLECTOR_SRCS := src/main_collector.c
COLLECTOR_OBJS := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(COLLECTOR_SRCS))

TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILDDIR)/%,$(TEST_SRCS))

# Binaries
BINS := $(BUILDDIR)/sv-subscriber $(BUILDDIR)/sv-capture-agent $(BUILDDIR)/sv-collector

.PHONY: all clean install test

all: $(BINS)

$(BUILDDIR)/sv-subscriber: $(SUBSCRIBER_OBJS) $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/sv-capture-agent: $(AGENT_OBJS) $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/sv-collector: $(COLLECTOR_OBJS) $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/%.o: src/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/%.o: tests/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/test_%: $(BUILDDIR)/test_%.o $(COMMON_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "=== Running $$t ==="; \
		$$t || exit 1; \
	done
	@echo "All tests passed."

install: $(BINS)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BINS) $(DESTDIR)$(BINDIR)/

clean:
	rm -rf $(BUILDDIR)

-include $(BUILDDIR)/*.d
