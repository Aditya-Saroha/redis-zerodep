CXX      := g++
CXXFLAGS := -O2 -std=gnu++17 -Wall -Wextra -pthread

SRC   := src/redis.cpp
BUILD := build

.PHONY: all clean test test-unit test-integration

all: $(BUILD)/redis-server $(BUILD)/redis-client

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/redis-server: $(SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) -DBUILD_SERVER -o $@ $<

$(BUILD)/redis-client: $(SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) -DBUILD_CLIENT -o $@ $<

$(BUILD)/test_skiplist: tests/test_skiplist.cpp $(SRC) tests/test_common.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BUILD)/test_hashtable: tests/test_hashtable.cpp $(SRC) tests/test_common.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BUILD)/test_zset: tests/test_zset.cpp $(SRC) tests/test_common.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $<

test-unit: $(BUILD)/test_skiplist $(BUILD)/test_hashtable $(BUILD)/test_zset
	$(BUILD)/test_skiplist
	$(BUILD)/test_hashtable
	$(BUILD)/test_zset

test-integration: all
	bash tests/integration_test.sh

test: test-unit test-integration

clean:
	rm -rf $(BUILD)