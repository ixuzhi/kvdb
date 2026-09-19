# Makefile for kvdb - a LevelDB-compatible storage engine in C
CC      := gcc
AR      := ar
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -g
INCLUDES := -Iinclude -Isrc
LDFLAGS  :=
# Extra flags (sanitizers, a different -O level) for callers such as
# scripts/run_sanitizers.sh. Passing CFLAGS/LDFLAGS on the make command line
# instead *replaces* them, and a command-line variable also swallows the `+=`
# appends below - so the POSIX feature-test macro and -lpthread silently
# vanish. SANFLAGS keeps the caller's input additive.
SANFLAGS :=

SRCDIR := src
OBJDIR := build
BINDIR := build

LIB_SRCS := $(SRCDIR)/port.c $(SRCDIR)/util.c $(SRCDIR)/env.c $(SRCDIR)/env_win.c \
            $(SRCDIR)/cache.c $(SRCDIR)/dbformat.c $(SRCDIR)/skiplist.c \
            $(SRCDIR)/memtable.c $(SRCDIR)/log.c $(SRCDIR)/write_batch.c \
            $(SRCDIR)/block.c $(SRCDIR)/filter_block.c $(SRCDIR)/table.c \
            $(SRCDIR)/two_level.c $(SRCDIR)/iterator.c $(SRCDIR)/snappy.c \
            $(SRCDIR)/version_edit.c $(SRCDIR)/version_set.c \
            $(SRCDIR)/table_cache.c $(SRCDIR)/db.c $(SRCDIR)/db_iter.c \
            $(SRCDIR)/repair.c $(SRCDIR)/env_mem.c $(SRCDIR)/c_api.c

TEST_SRCS := tests/test_main.c tests/test_util.c tests/test_cache.c \
             tests/test_log.c tests/test_batch.c tests/test_skiplist.c \
             tests/test_table.c tests/test_db.c tests/test_c_api.c \
             tests/test_format_extra.c tests/test_recovery_extra.c \
             tests/test_api_extra.c

# Pick the Env backend by compiler target, not host OS: MSYS2 runs on
# Windows but targets the POSIX emulation (no _WIN32, no windows.h).
# Must run before LIB_OBJS below expands LIB_SRCS.
TARGET_TRIPLET := $(shell $(CC) -dumpmachine 2>/dev/null)
# Two Windows triplets: mingw-w64 gcc (x86_64-w64-mingw32) and MSYS2's clang
# (x86_64-w64-windows-gnu). Only the former links statically - clang's ASan
# runtime is delivered as a DLL that must stay dynamically resolvable.
ifneq (,$(findstring windows,$(TARGET_TRIPLET)))
else ifneq (,$(findstring mingw,$(TARGET_TRIPLET)))
  LDFLAGS += -static
else
  LDFLAGS += -lpthread
  LIB_SRCS += $(SRCDIR)/env_posix.c
  CFLAGS += -D_GNU_SOURCE
endif

# Appended after the platform branch so the caller's -O1 wins over the default
# -O2 (the compiler honours the last -O on the command line).
CFLAGS  += $(SANFLAGS)
LDFLAGS += $(SANFLAGS)

LIB_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIB_SRCS))
TEST_OBJS := $(patsubst tests/%.c,$(OBJDIR)/tests/%.o,$(filter tests/%,$(TEST_SRCS)))

LIB := $(BINDIR)/libleveldb.a
TESTBIN := $(BINDIR)/kvdb_tests

.PHONY: all clean test

all: $(LIB) $(TESTBIN)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(BINDIR)
	$(AR) rcs $@ $^

$(OBJDIR)/%.o: $(SRCDIR)/%.c src/kvdb.h src/port.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR)/tests/%.o: tests/%.c tests/harness.h src/kvdb.h include/leveldb/c.h | $(OBJDIR)/tests
	$(CC) $(CFLAGS) $(INCLUDES) -Itests -c $< -o $@

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/tests:
	@mkdir -p $(OBJDIR)/tests

$(TESTBIN): $(TEST_OBJS) $(LIB)
	$(CC) $(CFLAGS) -Itests $(TEST_OBJS) $(LIB) -o $@ $(LDFLAGS)

test: $(TESTBIN)
	./$(TESTBIN)

# Removes this Makefile's own outputs only. `rm -rf $(OBJDIR)` was the earlier
# form, and with the default OBJDIR=build it also deleted the cross-engine
# evidence trees (build/interop/*, build/san-runs/*) that nothing else can
# reproduce. rmdir succeeds just on an emptied, otherwise-unused object dir.
clean:
	rm -f $(LIB_OBJS) $(TEST_OBJS) $(LIB) $(TESTBIN) $(TESTBIN).exe
	-rmdir $(OBJDIR)/tests $(OBJDIR) 2>/dev/null
