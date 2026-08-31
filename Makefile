# GNU Make
#
#   make                  quiet (normal)
#   make debug            debug logs (-DDEBUG)
#   make release          -O2; bump MINOR if main.c or src/**/*.c changed
#   make release update   bump MAJOR (x.0.0), even if src unchanged
#   make optimize / asan
#   make clean / distclean / help
#   V=1
#
# Version sources: main.c + src/**/*.c. test/ does not count.
# No GitHub tagging from here.

SHELL := /bin/bash
.SUFFIXES:
.DELETE_ON_ERROR:

CC     ?= gcc
SLANGC ?= slangc
STD := -std=gnu23
INC := -Iinclude

WARN := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
        -Wmissing-prototypes -Wformat=2 -Wcast-qual -Wwrite-strings

DEPFLAGS := -MMD -MP

PKG_VULKAN_CFLAGS := $(shell pkg-config --cflags vulkan 2>/dev/null)
PKG_VULKAN_LIBS   := $(shell pkg-config --libs   vulkan 2>/dev/null)
PKG_DRM_CFLAGS    := $(shell pkg-config --cflags libdrm  2>/dev/null)
PKG_FT_CFLAGS     := $(shell pkg-config --cflags freetype2 2>/dev/null)
PKG_FT_LIBS       := $(shell pkg-config --libs   freetype2 2>/dev/null)

BUILD ?= normal

ifeq ($(BUILD),normal)
  OPT  := -O1 -g
  DEFS := -DWP_BUILD_TYPE=\"normal\"
else ifeq ($(BUILD),debug)
  OPT  := -O0 -g3 -fno-omit-frame-pointer
  DEFS := -DDEBUG=1 -DWP_BUILD_TYPE=\"debug\"
else ifeq ($(BUILD),release)
  OPT  := -O2 -g1
  DEFS := -DNDEBUG -DWP_BUILD_TYPE=\"release\"
else ifeq ($(BUILD),optimize)
  OPT  := -O3 -march=native -flto=auto -fno-plt -fno-semantic-interposition
  DEFS := -DNDEBUG -DWP_BUILD_TYPE=\"optimize\"
else ifeq ($(BUILD),asan)
  OPT  := -O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined
  DEFS := -DDEBUG=1 -DWP_BUILD_TYPE=\"asan\"
else
  $(error unknown BUILD='$(BUILD)' (use normal|debug|release|optimize|asan))
endif

ifeq ($(filter update,$(MAKECMDGOALS)),update)
  BUMP_KIND := major
else
  BUMP_KIND ?= minor
endif

SRC_DIR    := src
BUILD_ROOT := build
BIN_DIR    := bin
OUT        := $(BUILD_ROOT)/$(BUILD)
BIN        := $(BIN_DIR)/$(BUILD)/wayplot

CFLAGS  := $(STD) -I$(OUT) --embed-dir=$(OUT) $(INC) $(WARN) $(OPT) $(DEFS) $(DEPFLAGS) \
           $(PKG_VULKAN_CFLAGS) $(PKG_DRM_CFLAGS) $(PKG_FT_CFLAGS)
LDFLAGS := $(OPT)
LDLIBS  := -lm $(PKG_VULKAN_LIBS) $(PKG_FT_LIBS)

APP_SRC := main.c
LIB_SRC := $(shell find $(SRC_DIR) -name '*.c' -type f 2>/dev/null | sort)
SRC     := $(APP_SRC) $(LIB_SRC)

APP_OBJ := $(OUT)/main.o
LIB_OBJ := $(patsubst $(SRC_DIR)/%.c,$(OUT)/src/%.o,$(LIB_SRC))
OBJ     := $(APP_OBJ) $(LIB_OBJ)
DEP     := $(OBJ:.o=.d)
# Test .d files are separate: omitting them left raster_test.o stale after
# session.h grew, so sizeof(wp_session) on the stack was too small (SSP abort).

V ?= 0
ifeq ($(V),0)
  Q := @
  log = @printf '  %-8s %s\n' '$(1)' '$(2)'
else
  Q :=
  log =
endif

TEST_URING_SRC := test/uring/ring_test.c
TEST_URING_OBJ := $(OUT)/test/uring/ring_test.o
TEST_URING_BIN := $(BIN_DIR)/$(BUILD)/test-uring
TEST_BENCH_SRC := test/uring/bench.c
TEST_BENCH_OBJ := $(OUT)/test/uring/bench.o
TEST_BENCH_BIN := $(BIN_DIR)/$(BUILD)/test-uring-bench
TEST_WL_SRC := test/wayland/conn_test.c
TEST_WL_OBJ := $(OUT)/test/wayland/conn_test.o
TEST_WL_BIN := $(BIN_DIR)/$(BUILD)/test-wayland-conn
TEST_PROTO_SRC := test/wayland/proto_test.c
TEST_PROTO_OBJ := $(OUT)/test/wayland/proto_test.o
TEST_PROTO_BIN := $(BIN_DIR)/$(BUILD)/test-wayland-proto
TEST_REG_SRC := test/wayland/registry_test.c
TEST_REG_OBJ := $(OUT)/test/wayland/registry_test.o
TEST_REG_BIN := $(BIN_DIR)/$(BUILD)/test-wayland-registry
TEST_SESS_SRC := test/wayland/session_test.c
TEST_SESS_OBJ := $(OUT)/test/wayland/session_test.o
TEST_SESS_BIN := $(BIN_DIR)/$(BUILD)/test-wayland-session
TEST_GPU_SRC := test/vulkan/gpu_test.c
TEST_GPU_OBJ := $(OUT)/test/vulkan/gpu_test.o
TEST_GPU_BIN := $(BIN_DIR)/$(BUILD)/test-vulkan-gpu
TEST_DEV_SRC := test/vulkan/device_test.c
TEST_DEV_OBJ := $(OUT)/test/vulkan/device_test.o
TEST_DEV_BIN := $(BIN_DIR)/$(BUILD)/test-vulkan-device
TEST_PRES_SRC := test/engine/present_test.c
TEST_PRES_OBJ := $(OUT)/test/engine/present_test.o
TEST_PRES_BIN := $(BIN_DIR)/$(BUILD)/test-engine-present
TEST_CUBE_SRC := test/renderer/cube_test.c
TEST_CUBE_OBJ := $(OUT)/test/renderer/cube_test.o
TEST_CUBE_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-cube
TEST_MATH_SRC := test/helper/math3d_test.c
TEST_MATH_OBJ := $(OUT)/test/helper/math3d_test.o
TEST_MATH_BIN := $(BIN_DIR)/$(BUILD)/test-math3d
TEST_MESH_SRC := test/renderer/mesh_test.c
TEST_MESH_OBJ := $(OUT)/test/renderer/mesh_test.o
TEST_MESH_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-mesh
TEST_OBJ_SRC := test/renderer/obj_test.c
TEST_OBJ_OBJ := $(OUT)/test/renderer/obj_test.o
TEST_OBJ_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-obj
TEST_DEM_SRC := test/renderer/dem_test.c
TEST_DEM_OBJ := $(OUT)/test/renderer/dem_test.o
TEST_DEM_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-dem
TEST_PLOT_SRC := test/renderer/plot_test.c
TEST_PLOT_OBJ := $(OUT)/test/renderer/plot_test.o
TEST_PLOT_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-plot
TEST_GRID_SRC := test/renderer/grid_test.c
TEST_GRID_OBJ := $(OUT)/test/renderer/grid_test.o
TEST_GRID_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-grid
TEST_IMAGE_SRC := test/renderer/image_test.c
TEST_IMAGE_OBJ := $(OUT)/test/renderer/image_test.o
TEST_IMAGE_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-image
TEST_SPV_SRC := test/renderer/spv_layout_test.c
TEST_SPV_OBJ := $(OUT)/test/renderer/spv_layout_test.o
TEST_SPV_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-spv
TEST_RAST_SRC := test/renderer/raster_test.c
TEST_RAST_OBJ := $(OUT)/test/renderer/raster_test.o
TEST_RAST_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-raster
TEST_FONT_SRC := test/renderer/font_test.c
TEST_FONT_OBJ := $(OUT)/test/renderer/font_test.o
TEST_FONT_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-font
TEST_TEXT_SRC := test/renderer/text_test.c
TEST_TEXT_OBJ := $(OUT)/test/renderer/text_test.o
TEST_TEXT_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-text
TEST_PASS_SRC := test/renderer/pass_test.c
TEST_PASS_OBJ := $(OUT)/test/renderer/pass_test.o
TEST_PASS_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-pass
TEST_DRAW_SRC := test/engine/draw_test.c
TEST_DRAW_OBJ := $(OUT)/test/engine/draw_test.o
TEST_DRAW_BIN := $(BIN_DIR)/$(BUILD)/test-engine-draw
TEST_CARD_SRC := test/renderer/card_test.c
TEST_CARD_OBJ := $(OUT)/test/renderer/card_test.o
TEST_CARD_BIN := $(BIN_DIR)/$(BUILD)/test-renderer-card
TEST_HIT_SRC := test/engine/hit_test.c
TEST_HIT_OBJ := $(OUT)/test/engine/hit_test.o
TEST_HIT_BIN := $(BIN_DIR)/$(BUILD)/test-engine-hit
TEST_VIEW_SRC := test/engine/view_test.c
TEST_VIEW_OBJ := $(OUT)/test/engine/view_test.o
TEST_VIEW_BIN := $(BIN_DIR)/$(BUILD)/test-engine-view
TEST_DOC_SRC := test/engine/doc_test.c
TEST_DOC_OBJ := $(OUT)/test/engine/doc_test.o
TEST_DOC_BIN := $(BIN_DIR)/$(BUILD)/test-engine-doc

LIT_VERT_SPV := $(OUT)/shaders/lit.vert.spv
LIT_FRAG_SPV := $(OUT)/shaders/lit.frag.spv
TEXT_VERT_SPV := $(OUT)/shaders/text.vert.spv
TEXT_FRAG_SPV := $(OUT)/shaders/text.frag.spv
CARD_VERT_SPV := $(OUT)/shaders/card.vert.spv
CARD_FRAG_SPV := $(OUT)/shaders/card.frag.spv

.PHONY: all normal debug release update optimize asan
.PHONY: bump-if-needed clean distclean help compile-commands test

all: $(BIN)

normal debug optimize asan:
	$(MAKE) BUILD=$@ all

# Submake bump first, then build, so VERSION is settled before version.h.
release:
	$(MAKE) BUILD=release BUMP_KIND=$(BUMP_KIND) bump-if-needed
	$(MAKE) BUILD=release all

update:
	@:

bump-if-needed:
	$(Q)chmod +x scripts/version-bump.sh
	$(Q)scripts/version-bump.sh $(BUMP_KIND)

$(OUT)/version.h: VERSION Makefile
	$(call log,GEN,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)awk -F. '{ \
	  gsub(/\r/, ""); \
	  printf "#ifndef WP_VERSION_H\n#define WP_VERSION_H\n\n"; \
	  printf "#define WP_VERSION_MAJOR %d\n", $$1+0; \
	  printf "#define WP_VERSION_MINOR %d\n", $$2+0; \
	  printf "#define WP_VERSION_PATCH %d\n", $$3+0; \
	  printf "#define WP_VERSION_STRING \"%s\"\n\n", $$0; \
	  printf "#endif\n"; \
	}' VERSION > $@

$(BIN): $(OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OUT)/main.o: main.c $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(LIT_VERT_SPV): shaders/lit.slang
	$(call log,SLANG,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(SLANGC) $< -target spirv -profile sm_6_0 -stage vertex -entry vs_main \
	  -matrix-layout-column-major -o $@

$(LIT_FRAG_SPV): shaders/lit.slang
	$(call log,SLANG,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(SLANGC) $< -target spirv -profile sm_6_0 -stage fragment -entry fs_main \
	  -matrix-layout-column-major -o $@

$(OUT)/src/renderer/lit.o: $(LIT_VERT_SPV) $(LIT_FRAG_SPV)

$(TEXT_VERT_SPV): shaders/text.slang
	$(call log,SLANG,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(SLANGC) $< -target spirv -profile sm_6_0 -stage vertex -entry vs_main \
	  -matrix-layout-column-major -o $@

$(TEXT_FRAG_SPV): shaders/text.slang
	$(call log,SLANG,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(SLANGC) $< -target spirv -profile sm_6_0 -stage fragment -entry fs_main \
	  -matrix-layout-column-major -o $@

$(OUT)/src/renderer/text.o: $(TEXT_VERT_SPV) $(TEXT_FRAG_SPV)

$(CARD_VERT_SPV): shaders/card.slang
	$(call log,SLANG,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(SLANGC) $< -target spirv -profile sm_6_0 -stage vertex -entry vs_main \
	  -matrix-layout-column-major -o $@

$(CARD_FRAG_SPV): shaders/card.slang
	$(call log,SLANG,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(SLANGC) $< -target spirv -profile sm_6_0 -stage fragment -entry fs_main \
	  -matrix-layout-column-major -o $@

$(OUT)/src/renderer/card.o: $(CARD_VERT_SPV) $(CARD_FRAG_SPV)

$(OUT)/src/%.o: $(SRC_DIR)/%.c $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/test/uring/ring_test.o: $(TEST_URING_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_URING_BIN): $(TEST_URING_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/uring/bench.o: $(TEST_BENCH_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_BENCH_BIN): $(TEST_BENCH_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/wayland/conn_test.o: $(TEST_WL_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_WL_BIN): $(TEST_WL_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/wayland/proto_test.o: $(TEST_PROTO_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_PROTO_BIN): $(TEST_PROTO_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/wayland/registry_test.o: $(TEST_REG_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_REG_BIN): $(TEST_REG_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/wayland/session_test.o: $(TEST_SESS_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_SESS_BIN): $(TEST_SESS_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/vulkan/gpu_test.o: $(TEST_GPU_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_GPU_BIN): $(TEST_GPU_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/vulkan/device_test.o: $(TEST_DEV_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_DEV_BIN): $(TEST_DEV_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/engine/present_test.o: $(TEST_PRES_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_PRES_BIN): $(TEST_PRES_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/cube_test.o: $(TEST_CUBE_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_CUBE_BIN): $(TEST_CUBE_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/helper/math3d_test.o: $(TEST_MATH_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_MATH_BIN): $(TEST_MATH_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/mesh_test.o: $(TEST_MESH_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_MESH_BIN): $(TEST_MESH_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/obj_test.o: $(TEST_OBJ_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_OBJ_BIN): $(TEST_OBJ_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/dem_test.o: $(TEST_DEM_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_DEM_BIN): $(TEST_DEM_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/plot_test.o: $(TEST_PLOT_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_PLOT_BIN): $(TEST_PLOT_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/grid_test.o: $(TEST_GRID_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_GRID_BIN): $(TEST_GRID_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/image_test.o: $(TEST_IMAGE_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_IMAGE_BIN): $(TEST_IMAGE_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/spv_layout_test.o: $(TEST_SPV_SRC) $(OUT)/version.h $(LIT_VERT_SPV) $(LIT_FRAG_SPV)
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_SPV_BIN): $(TEST_SPV_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/raster_test.o: $(TEST_RAST_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_RAST_BIN): $(TEST_RAST_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/font_test.o: $(TEST_FONT_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_FONT_BIN): $(TEST_FONT_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/text_test.o: $(TEST_TEXT_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_TEXT_BIN): $(TEST_TEXT_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/pass_test.o: $(TEST_PASS_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_PASS_BIN): $(TEST_PASS_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/engine/draw_test.o: $(TEST_DRAW_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_DRAW_BIN): $(TEST_DRAW_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/renderer/card_test.o: $(TEST_CARD_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_CARD_BIN): $(TEST_CARD_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/engine/hit_test.o: $(TEST_HIT_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_HIT_BIN): $(TEST_HIT_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/engine/view_test.o: $(TEST_VIEW_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_VIEW_BIN): $(TEST_VIEW_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/test/engine/doc_test.o: $(TEST_DOC_SRC) $(OUT)/version.h
	$(call log,CC,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_DOC_BIN): $(TEST_DOC_OBJ) $(LIB_OBJ)
	$(call log,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

define run_test
	$(call log,TEST,$(1))
	$(Q)set -o pipefail && $(1) | tee $(2)
endef

test: $(BIN) $(TEST_URING_BIN) $(TEST_PROTO_BIN) $(TEST_WL_BIN) $(TEST_REG_BIN) $(TEST_SESS_BIN) $(TEST_GPU_BIN) $(TEST_DEV_BIN) $(TEST_PRES_BIN) $(TEST_CUBE_BIN) $(TEST_MATH_BIN) $(TEST_MESH_BIN) $(TEST_OBJ_BIN) $(TEST_DEM_BIN) $(TEST_PLOT_BIN) $(TEST_GRID_BIN) $(TEST_IMAGE_BIN) $(TEST_SPV_BIN) $(TEST_RAST_BIN) $(TEST_FONT_BIN) $(TEST_TEXT_BIN) $(TEST_PASS_BIN) $(TEST_DRAW_BIN) $(TEST_CARD_BIN) $(TEST_HIT_BIN) $(TEST_VIEW_BIN) $(TEST_DOC_BIN) $(TEST_BENCH_BIN)
	$(Q)mkdir -p $(BUILD_ROOT)
	$(call run_test,$(TEST_URING_BIN),$(BUILD_ROOT)/test-uring.log)
	$(call run_test,$(TEST_MATH_BIN),$(BUILD_ROOT)/test-math3d.log)
	$(call run_test,$(TEST_MESH_BIN),$(BUILD_ROOT)/test-renderer-mesh.log)
	$(call run_test,$(TEST_OBJ_BIN),$(BUILD_ROOT)/test-renderer-obj.log)
	$(call run_test,$(TEST_DEM_BIN),$(BUILD_ROOT)/test-renderer-dem.log)
	$(call run_test,$(TEST_PLOT_BIN),$(BUILD_ROOT)/test-renderer-plot.log)
	$(call run_test,$(TEST_GRID_BIN),$(BUILD_ROOT)/test-renderer-grid.log)
	$(call run_test,$(TEST_IMAGE_BIN),$(BUILD_ROOT)/test-renderer-image.log)
	$(call run_test,$(TEST_SPV_BIN),$(BUILD_ROOT)/test-renderer-spv.log)
	$(call run_test,$(TEST_RAST_BIN),$(BUILD_ROOT)/test-renderer-raster.log)
	$(call run_test,$(TEST_FONT_BIN),$(BUILD_ROOT)/test-renderer-font.log)
	$(call run_test,$(TEST_TEXT_BIN),$(BUILD_ROOT)/test-renderer-text.log)
	$(call run_test,$(TEST_PASS_BIN),$(BUILD_ROOT)/test-renderer-pass.log)
	$(call run_test,$(TEST_DRAW_BIN),$(BUILD_ROOT)/test-engine-draw.log)
	$(call run_test,$(TEST_CARD_BIN),$(BUILD_ROOT)/test-renderer-card.log)
	$(call run_test,$(TEST_HIT_BIN),$(BUILD_ROOT)/test-engine-hit.log)
	$(call run_test,$(TEST_VIEW_BIN),$(BUILD_ROOT)/test-engine-view.log)
	$(call run_test,$(TEST_DOC_BIN),$(BUILD_ROOT)/test-engine-doc.log)
	$(call run_test,$(TEST_PROTO_BIN),$(BUILD_ROOT)/test-wayland-proto.log)
	$(call run_test,$(TEST_WL_BIN),$(BUILD_ROOT)/test-wayland-conn.log)
	$(call run_test,$(TEST_REG_BIN),$(BUILD_ROOT)/test-wayland-registry.log)
	$(call run_test,$(TEST_SESS_BIN),$(BUILD_ROOT)/test-wayland-session.log)
	$(call run_test,$(TEST_GPU_BIN),$(BUILD_ROOT)/test-vulkan-gpu.log)
	$(call run_test,$(TEST_DEV_BIN),$(BUILD_ROOT)/test-vulkan-device.log)
	$(call run_test,$(TEST_PRES_BIN),$(BUILD_ROOT)/test-engine-present.log)
	$(call run_test,$(TEST_CUBE_BIN),$(BUILD_ROOT)/test-renderer-cube.log)
	$(call run_test,$(TEST_BENCH_BIN),$(BUILD_ROOT)/test-uring-bench.log)

-include $(DEP)
-include $(wildcard $(OUT)/test/*/*.d)

clean:
	rm -rf $(OUT) $(BIN_DIR)/$(BUILD)

distclean:
	rm -rf $(BUILD_ROOT) compile_commands.json
	find $(BIN_DIR) -mindepth 1 ! -name '.gitkeep' -exec rm -rf {} + 2>/dev/null || true

help:
	@printf '%s\n' \
	  '  make                 quiet  (-O1 -g)' \
	  '  make debug           debug logs (-O0 -g3 -DDEBUG)' \
	  '  make release         -O2; MINOR++ if main.c or src/**/*.c changed' \
	  '  make release update  MAJOR++ (x.0.0), even if src unchanged' \
	  '  make optimize        -O3 -march=native -flto' \
	  '  make asan            ASan+UBSan + debug logs' \
	  '  make clean           this BUILD tree' \
	  '  make distclean       all build trees' \
	  '  make test            uring + math3d + mesh + obj + dem + plot + image + spv + raster + font + text + pass + draw + card + hit + view + doc + proto + conn + registry + session + gpu + device + present + cube + bench' \
	  '  make compile-commands' \
	  '  V=1                  full commands' \
	  '' \
	  "  VERSION $$(tr -d '\n' < VERSION)   binary $(BIN)"

compile-commands: compile_commands.json

compile_commands.json: $(SRC) Makefile
	$(call log,GEN,$@)
	$(Q){ \
	  echo '['; \
	  first=1; \
	  for f in $(SRC); do \
	    [ $$first -eq 1 ] || echo ','; \
	    first=0; \
	    abs="$$(pwd)/$$f"; \
	    printf '  {"directory": "%s", "file": "%s", "arguments": [' "$$(pwd)" "$$abs"; \
	    printf '"%s"' $(CC); \
	    for a in $(CFLAGS) -c -o /dev/null $$f; do \
	      printf ', "%s"' "$$a"; \
	    done; \
	    printf ']}'; \
	  done; \
	  echo; echo ']'; \
	} > $@
