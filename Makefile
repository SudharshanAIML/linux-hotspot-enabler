# ============================================================================
# Linux Hotspot Enabler - Makefile
# Simultaneous WiFi + Hotspot for Linux
# ============================================================================

CC       := gcc
CFLAGS   := -Wall -Wextra -Wno-unused-parameter -std=c11 -D_GNU_SOURCE
LDFLAGS  := -lncurses -lpthread

SRC_DIR  := src
INC_DIR  := include
BUILD_DIR := build

SOURCES  := $(wildcard $(SRC_DIR)/*.c)
OBJECTS  := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SOURCES))
TARGET   := hotspot-enabler

PREFIX   := /usr/local

.PHONY: all clean install uninstall

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo ""
	@echo "  ✅ Build successful: ./$(TARGET)"
	@echo "  Run with: sudo ./$(TARGET)"
	@echo ""

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
	@echo "  🧹 Cleaned build artifacts"

install: $(TARGET)
	install -Dm755 $(TARGET) $(PREFIX)/bin/$(TARGET)
	@echo "  📦 Installed to $(PREFIX)/bin/$(TARGET)"

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	@echo "  🗑️  Uninstalled from $(PREFIX)/bin/$(TARGET)"
