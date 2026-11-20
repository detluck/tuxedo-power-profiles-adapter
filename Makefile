CC       := gcc
TARGET   := tcc-power-profiles

BUILD_DIR := build

# Source files
SOURCES := \
	main.c \
	profile_mapping.c \
	$(wildcard tcc/*.c) \
  $(wildcard ppd/*.c)

OBJECTS := $(SOURCES:%.c=$(BUILD_DIR)/%.o)
DEPS    := $(OBJECTS:.o=.d)

# Dependencies
PKG_CONFIG := pkg-config
PACKAGES   := libsystemd json-c

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS   += -Wall -Wextra -Wpedantic -std=c11 -g -MMD -MP
LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PACKAGES))

# Config
CONFIG_TEMPLATE := config/config.json
CONFIG_DIR      := /etc/tcc-power-profiles
CONFIG_FILE     := $(CONFIG_DIR)/config.json

.PHONY: all clean install-config

all: $(TARGET) install-config

# Link
$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

# Compile
$(BUILD_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Install default config only if it doesn't exist
install-config:
	@if [ -f "$(CONFIG_FILE)" ]; then \
		echo "Config already exists. Keeping it."; \
	else \
		echo "Installing default config..."; \
		install -Dm644 "$(CONFIG_TEMPLATE)" "$(CONFIG_FILE)"; \
	fi

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(DEPS)
