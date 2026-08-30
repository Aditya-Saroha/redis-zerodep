CXX      := g++
CXXFLAGS := -O2 -std=gnu++17 -Wall -Wextra -pthread -frandom-seed=zerodep -ffile-prefix-map=$(CURDIR)=.

BUILD := build

TARGET_SERVER := $(BUILD)/redis-server
TARGET_CLIENT := $(BUILD)/redis-client

.PHONY: all clean test test-unit test-integration reproduce-proof

all: $(TARGET_SERVER) $(TARGET_CLIENT)

$(BUILD):
	mkdir -p $(BUILD)

$(TARGET_SERVER): src/server.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(TARGET_CLIENT): src/client.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BUILD)/test_skiplist: tests/test_skiplist.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $<

$(BUILD)/test_hashtable: tests/test_hashtable.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $<

$(BUILD)/test_zset: tests/test_zset.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $<

test-unit: $(BUILD)/test_skiplist $(BUILD)/test_hashtable $(BUILD)/test_zset
	$(BUILD)/test_skiplist
	$(BUILD)/test_hashtable
	$(BUILD)/test_zset

test-integration: all
	bash tests/integration_test.sh

test: test-unit test-integration

reproduce-proof:
	@echo "--- Build Run 1 ---"
	$(MAKE) clean && $(MAKE) all
	sha256sum $(TARGET_SERVER) > hash1.txt
	cat hash1.txt
	@echo "--- Build Run 2 ---"
	$(MAKE) clean && $(MAKE) all
	sha256sum $(TARGET_SERVER) > hash2.txt
	cat hash2.txt
	@echo "--- Verifying ---"
	cmp hash1.txt hash2.txt && echo "SUCCESS: Binaries are byte-identical!" || echo "FAIL: Hashes differ."
	@rm hash1.txt hash2.txt

clean:
	rm -rf $(BUILD)