CC ?= gcc
SLANGC ?= slangc
SLANGFLAGS ?= -target spirv -profile sm_6_6+SPIRV_1_6 -matrix-layout-column-major

VULKAN_CFLAGS ?= $(shell pkg-config --cflags vulkan libdrm 2>/dev/null || echo "-I/usr/include/libdrm")
VULKAN_LIBS ?= $(shell pkg-config --libs vulkan libdrm 2>/dev/null || echo "-lvulkan -ldrm") -lm

BIN_DIR = build
SAN_DIR = $(BIN_DIR)/san
LOG_DIR = logs
TEST_LOG_DIR = $(LOG_DIR)/test_logs
TEST_LOG = $(TEST_LOG_DIR)/test_results.log
FAIL_LOG = $(TEST_LOG_DIR)/failures.log

CFLAGS ?= -std=c23 -D_GNU_SOURCE -O3 -pthread -Wall -Wextra -Wpedantic -Werror=vla -Iinclude --embed-dir=$(BIN_DIR) $(VULKAN_CFLAGS) -MMD -MP
TEST_CFLAGS ?= $(CFLAGS) -Itests

# Sanitizer Flags (AddressSanitizer + UndefinedBehaviorSanitizer)
SAN_CFLAGS ?= -std=c23 -D_GNU_SOURCE -O1 -g3 -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror=vla -Iinclude --embed-dir=$(BIN_DIR) $(VULKAN_CFLAGS) -MMD -MP
SAN_TEST_CFLAGS ?= $(SAN_CFLAGS) -Itests

TARGET = $(BIN_DIR)/engine
TEST_TARGET = $(BIN_DIR)/test_runner
SAN_TEST_TARGET = $(SAN_DIR)/test_runner_san

# Shader source and output definitions
SHADER_DIR = shaders
SPV_DIR = $(BIN_DIR)/shaders

SPV_TARGETS = $(SPV_DIR)/card.vert.spv $(SPV_DIR)/card.frag.spv \
              $(SPV_DIR)/plot.vert.spv $(SPV_DIR)/plot.frag.spv \
              $(SPV_DIR)/mesh.vert.spv $(SPV_DIR)/mesh.frag.spv

# Recursive source discovery
SRCS = $(wildcard src/*.c) $(wildcard src/**/*.c)
OBJS = $(patsubst src/%.c, $(BIN_DIR)/%.o, $(SRCS))
SAN_OBJS = $(patsubst src/%.c, $(SAN_DIR)/%.o, $(SRCS))

TEST_SRCS = $(wildcard tests/*.c)
TEST_OBJS = $(patsubst tests/%.c, $(BIN_DIR)/%.o, $(TEST_SRCS))
SAN_TEST_OBJS = $(patsubst tests/%.c, $(SAN_DIR)/%.o, $(TEST_SRCS))

DEPS = $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(BIN_DIR)/main.d \
       $(SAN_OBJS:.o=.d) $(SAN_TEST_OBJS:.o=.d)

.PHONY: all clean run test lint sanitize shaders

all: $(TARGET)

shaders: $(SPV_TARGETS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(SAN_DIR):
	mkdir -p $(SAN_DIR)

$(LOG_DIR):
	mkdir -p $(LOG_DIR)

$(TEST_LOG_DIR): | $(LOG_DIR)
	mkdir -p $(TEST_LOG_DIR)

$(SPV_DIR):
	mkdir -p $(SPV_DIR)

# Slang compiler rules
$(SPV_DIR)/%.vert.spv: $(SHADER_DIR)/%.slang | $(SPV_DIR)
	@mkdir -p $(dir $@)
	$(SLANGC) $< $(SLANGFLAGS) -stage vertex -entry vs_main -o $@

$(SPV_DIR)/%.frag.spv: $(SHADER_DIR)/%.slang | $(SPV_DIR)
	@mkdir -p $(dir $@)
	$(SLANGC) $< $(SLANGFLAGS) -stage fragment -entry fs_main -o $@

# Shaders must be compiled before graphics pipeline object files (for C23 #embed)
$(BIN_DIR)/gfx/pipeline.o: $(SPV_TARGETS)
$(SAN_DIR)/gfx/pipeline.o: $(SPV_TARGETS)

# Standard build recipes
$(BIN_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(BIN_DIR)/main.o: main.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(BIN_DIR)/main.o $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(VULKAN_LIBS)

$(TEST_TARGET): $(TEST_OBJS) $(OBJS) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(TEST_OBJS) $(OBJS) -o $@ $(VULKAN_LIBS)

# Sanitizer build recipes
$(SAN_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SAN_CFLAGS) -c $< -o $@

$(SAN_DIR)/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SAN_TEST_CFLAGS) -c $< -o $@

$(SAN_TEST_TARGET): $(SAN_TEST_OBJS) $(SAN_OBJS) | $(SAN_DIR)
	$(CC) $(SAN_TEST_CFLAGS) $(SAN_TEST_OBJS) $(SAN_OBJS) -o $@ $(VULKAN_LIBS)

-include $(DEPS)

test: $(TEST_TARGET) | $(TEST_LOG_DIR)
	@./$(TEST_TARGET) 2>&1 | tee $(TEST_LOG)
	@cp $(TEST_LOG) $(TEST_LOG_DIR)/test_$$(date +%Y%m%d_%H%M%S).log
	@if [ -s $(FAIL_LOG) ]; then \
		echo " [!] Detailed failure log recorded at: $(FAIL_LOG)"; \
	fi

sanitize: $(SAN_TEST_TARGET) | $(TEST_LOG_DIR)
	@echo "Executing tests under AddressSanitizer & UndefinedBehaviorSanitizer..."
	@./$(SAN_TEST_TARGET) 2>&1 | tee $(TEST_LOG_DIR)/sanitize_results.log

lint:
	@echo "Auditing codebase against architectural rules..."
	@printf "  %-50s " "Checking banned headers (<stdbool.h>, <stdalign.h>)..."
	@if grep -rnE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<std(bool|align)\.h>' include/ src/ tests/ main.c 2>/dev/null; then \
		printf "\033[91mFAILED\033[0m\n"; exit 1; \
	else \
		printf "\033[92mPASSED\033[0m\n"; \
	fi
	@printf "  %-50s " "Checking banned I/O interfaces (liburing, epoll, poll)..."
	@if grep -rnE "liburing\.h|<sys/epoll\.h>|<poll\.h>|epoll_create|epoll_ctl|epoll_wait|poll\(" include/ src/ tests/ main.c 2>/dev/null; then \
		printf "\033[91mFAILED\033[0m\n"; exit 1; \
	else \
		printf "\033[92mPASSED\033[0m\n"; \
	fi
	@printf "  %-50s " "Checking banned graphics libraries (libwayland, OpenGL)..."
	@if grep -rnE "wayland-client\.h|<GL/|<GLES/|<EGL/|glBegin|glDraw" include/ src/ tests/ main.c 2>/dev/null; then \
		printf "\033[91mFAILED\033[0m\n"; exit 1; \
	else \
		printf "\033[92mPASSED\033[0m\n"; \
	fi
	@printf "\033[92m✔ All architectural rules verified successfully.\033[0m\n"

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BIN_DIR) $(LOG_DIR) test_results.log
