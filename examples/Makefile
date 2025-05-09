CC = gcc
# Get GStreamer CFLAGS and LIBS using pkg-config
GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0)
GST_LIBS := $(shell pkg-config --libs gstreamer-1.0)

# Add any other custom CFLAGS (e.g., -Wall for warnings) or LDFLAGS
CFLAGS = -Wall $(GST_CFLAGS)
LDFLAGS = $(GST_LIBS)

# Output directory for executables, relative to this Makefile
BIN_DIR = bin

# Automatically find all .c files in the current directory
EXAMPLE_SOURCES = $(wildcard *.c)

# Generate executable names by replacing .c with an executable in BIN_DIR
# Ensure that if EXAMPLE_SOURCES is empty, EXAMPLE_EXECUTABLES is also empty
EXAMPLE_EXECUTABLES = $(patsubst %.c,$(BIN_DIR)/%,$(EXAMPLE_SOURCES))

# Default target: build all examples
.PHONY: all
all: $(EXAMPLE_EXECUTABLES)

# Rule to compile each C source file into an executable in the BIN_DIR
# $< is the source file (e.g., example1.c)
# $@ is the target file (e.g., bin/example1)
$(BIN_DIR)/%: %.c
	@mkdir -p $(@D) # Use $(@D) to get the directory part of the target (e.g., bin)
	@echo "Compiling $< -> $@"
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

# Target to clean up compiled files
.PHONY: clean
clean:
	@echo "Cleaning up compiled examples from $(BIN_DIR)..."
	@rm -rf $(BIN_DIR)