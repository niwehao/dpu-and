# DPU Memory Defragmentation Makefile
# Standalone build system for userspace testing

CC = gcc
CFLAGS = -Wall -Wextra -Werror -Iinclude -g -O2
LDFLAGS =

# Directories
SRCDIR = src
INCDIR = include
TESTDIR = tests
BUILDDIR = build
BINDIR = bin

# Source files
SRCS = $(SRCDIR)/dpu_defrag.c
OBJS = $(BUILDDIR)/dpu_defrag.o

# Targets
DEMO = $(BINDIR)/dpu_demo
TEST = $(BINDIR)/test_defrag

# Default target
all: directories $(DEMO) $(TEST)

# Create necessary directories
directories:
	@mkdir -p $(BUILDDIR)
	@mkdir -p $(BINDIR)

# Build demo program
$(DEMO): $(OBJS) $(BUILDDIR)/main.o
	@echo "Linking demo program..."
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $(DEMO)"

# Build test program
$(TEST): $(OBJS) $(BUILDDIR)/test_defrag.o
	@echo "Linking test program..."
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $(TEST)"

# Compile object files
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/%.o: $(TESTDIR)/%.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c -o $@ $<

# Run demo
demo: $(DEMO)
	@echo ""
	@echo "========================================="
	@echo "Running demo..."
	@echo "========================================="
	@./$(DEMO)

# Run tests
test: $(TEST)
	@echo ""
	@echo "========================================="
	@echo "Running tests..."
	@echo "========================================="
	@./$(TEST)

# Run tests with valgrind (memory leak detection)
valgrind: $(TEST)
	@echo ""
	@echo "========================================="
	@echo "Running tests with Valgrind..."
	@echo "========================================="
	@valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TEST)

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILDDIR) $(BINDIR)
	@echo "Clean complete."

# Clean and rebuild
rebuild: clean all

# Install (optional - for system-wide installation)
install: all
	@echo "Installing to /usr/local/bin..."
	@sudo cp $(DEMO) /usr/local/bin/
	@sudo cp $(TEST) /usr/local/bin/
	@echo "Installation complete."

# Uninstall
uninstall:
	@echo "Uninstalling from /usr/local/bin..."
	@sudo rm -f /usr/local/bin/dpu_demo
	@sudo rm -f /usr/local/bin/test_defrag
	@echo "Uninstallation complete."

# Help
help:
	@echo "DPU Defragmentation Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all        - Build all programs (default)"
	@echo "  demo       - Build and run demo program"
	@echo "  test       - Build and run test suite"
	@echo "  valgrind   - Run tests with Valgrind memory checker"
	@echo "  clean      - Remove all build artifacts"
	@echo "  rebuild    - Clean and rebuild everything"
	@echo "  install    - Install binaries to /usr/local/bin"
	@echo "  uninstall  - Remove installed binaries"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Example usage:"
	@echo "  make          # Build everything"
	@echo "  make test     # Build and run tests"
	@echo "  make demo     # Build and run demo"
	@echo "  make clean    # Clean build files"

# Phony targets
.PHONY: all directories demo test valgrind clean rebuild install uninstall help

# Dependencies
$(BUILDDIR)/dpu_defrag.o: $(SRCDIR)/dpu_defrag.c $(INCDIR)/dpu_defrag.h $(INCDIR)/list.h
$(BUILDDIR)/main.o: $(SRCDIR)/main.c $(INCDIR)/dpu_defrag.h $(INCDIR)/list.h
$(BUILDDIR)/test_defrag.o: $(TESTDIR)/test_defrag.c $(INCDIR)/dpu_defrag.h $(INCDIR)/list.h
