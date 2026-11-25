CC       := gcc
TARGET   := tcc-power-profiles

BUILD_DIR := build

# Source files
SOURCES := \
	main.c \
	$(wildcard config/*.c) \
	$(wildcard tcc/*.c) \
	$(wildcard ppd/*.c)

OBJECTS := $(SOURCES:%.c=$(BUILD_DIR)/%.o)
DEPS    := $(OBJECTS:.o=.d)

# Dependencies
PKG_CONFIG := pkg-config
PACKAGES   := libsystemd json-c

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS   += -Wall -Wextra -Wpedantic -std=gnu11 -g -MMD -MP
LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PACKAGES))

# Install locations
PREFIX      ?= /usr
DESTDIR     ?=
BINDIR      := $(DESTDIR)$(PREFIX)/bin
DBUS_DIR    := $(DESTDIR)$(PREFIX)/share/dbus-1/system.d

# Config
CONFIG_TEMPLATE := config/config.json
CONFIG_DIR      := $(DESTDIR)/etc/tcc-power-profiles
CONFIG_FILE     := $(CONFIG_DIR)/config.json

# D-Bus policy
DBUS_POLICY := dbus/org.freedesktop.UPower.PowerProfiles.conf

.PHONY: all clean install install-config uninstall

all: $(TARGET)

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

install: $(TARGET) install-config
	install -Dm755 $(TARGET) $(BINDIR)/$(TARGET)
	install -Dm644 $(DBUS_POLICY) $(DBUS_DIR)/$(notdir $(DBUS_POLICY))

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(DBUS_DIR)/$(notdir $(DBUS_POLICY))
	# config is left in place on purpose, it may contain user edits

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(DEPS)
