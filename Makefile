CXX      ?= g++
CXXFLAGS ?= -O2
LDFLAGS  ?=
WARN     := -std=c++17 -Wall -Wextra -Wpedantic -Wshadow

BUILD    := build
TARGET   := filexfer

APP_SRC  := $(wildcard src/*.cpp)
# Юнит-тесты собираются только из кода, не зависящего от сокетов
TEST_SRC := tests/unit_tests.cpp src/util.cpp src/protocol.cpp src/fault_transport.cpp

APP_OBJ  := $(APP_SRC:%.cpp=$(BUILD)/%.o)
TEST_OBJ := $(TEST_SRC:%.cpp=$(BUILD)/%.o)
UNIT     := $(BUILD)/unit_tests

SANITIZE := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined

.PHONY: all unit asan test clean

all: $(TARGET)

$(TARGET): $(APP_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^

unit: $(UNIT)

$(UNIT): $(TEST_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(WARN) $(CXXFLAGS) -Isrc -MMD -MP -c -o $@ $<

# Сборка с AddressSanitizer и UndefinedBehaviorSanitizer (программа и юнит-тесты)
asan:
	$(MAKE) clean
	$(MAKE) all unit CXXFLAGS="$(SANITIZE)" LDFLAGS="-fsanitize=address,undefined"

test: all unit
	$(UNIT)
	bash tests/run_tests.sh ./$(TARGET)

clean:
	rm -rf $(BUILD) $(TARGET) tests/tmp

-include $(APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
