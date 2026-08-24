CC ?= cc
VERSION ?= 0.1.2
PREFIX ?= /usr/local
SYSCONFDIR ?= /etc
BUILD_DIR ?= build

CPPFLAGS += -Iinclude -Ithird_party/cJSON -DAG_VERSION=\"$(VERSION)\"
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -lm

APP_SOURCES = src/main.c src/config.c src/policy.c src/log.c src/flex.c third_party/cJSON/cJSON.c
TEST_SOURCES = tests/test_core.c src/config.c src/policy.c third_party/cJSON/cJSON.c
APP = $(BUILD_DIR)/antennaguardian-pilite
CORE_TEST = $(BUILD_DIR)/test-core

.PHONY: all clean test install

all: $(APP)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(APP): $(APP_SOURCES) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(APP_SOURCES) $(LDLIBS)

$(CORE_TEST): $(TEST_SOURCES) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_SOURCES) $(LDLIBS)

test: $(APP) $(CORE_TEST)
	./$(CORE_TEST)
	python3 tests/test_fake_radio.py ./$(APP)

install: $(APP)
	install -D -m 0755 $(APP) $(DESTDIR)$(PREFIX)/bin/antennaguardian-pilite
	install -D -m 0644 config/config.example.json \
		$(DESTDIR)$(SYSCONFDIR)/antennaguardian-pilite/config.example.json
	install -D -m 0644 packaging/antennaguardian-pilite.service \
		$(DESTDIR)$(SYSCONFDIR)/systemd/system/antennaguardian-pilite.service

clean:
	rm -rf $(BUILD_DIR) dist
