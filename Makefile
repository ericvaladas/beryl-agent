CROSS    := i686-w64-mingw32
CXX      := $(CROSS)-g++
CC       := $(CROSS)-gcc
AR       := $(CROSS)-gcc-ar
STRIP    := $(CROSS)-strip

SRC_DIR   := src
BUILD_DIR := build
DEPS_DIR  := deps

COMMON_DEFS := -DWIN32_LEAN_AND_MEAN -DMG_ENABLE_LOG=0 -D__MINGW32__

CXXFLAGS := -std=c++14 -Os -Wall -fno-rtti \
            -ffunction-sections -fdata-sections -flto \
            $(COMMON_DEFS)

CFLAGS := -Os -Wall -ffunction-sections -fdata-sections \
          $(COMMON_DEFS)

MONGOOSE_DEFS := -DMG_ENABLE_DIRLIST=0 -DMG_ENABLE_POSIX_FS=0 \
                 -DMG_ENABLE_MD5=0 -DMG_ENABLE_SSI=0 \
                 -DMG_ENABLE_PACKED_FS=0 -DMG_ENABLE_PROFILE=0

INCLUDES := -I$(DEPS_DIR)/Detours/src

LDFLAGS := -shared -static -Wl,--gc-sections,--enable-stdcall-fixup -flto \
           $(SRC_DIR)/wininet.def \
           -lws2_32 -lmswsock -lkernel32

# Detours sources
DETOURS_SRC := $(addprefix $(DEPS_DIR)/Detours/src/, detours.cpp modules.cpp disasm.cpp)
DETOURS_OBJ := $(patsubst $(DEPS_DIR)/Detours/src/%.cpp, $(BUILD_DIR)/detours/%.o, $(DETOURS_SRC))
DETOURS_LIB := $(BUILD_DIR)/libdetours.a

# C library objects (mongoose)
MONGOOSE_OBJ := $(BUILD_DIR)/mongoose.o

# Project sources
SRCS := $(SRC_DIR)/dllmain.cpp $(SRC_DIR)/wininet_proxy.cpp
OBJS := $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))

TARGET := $(BUILD_DIR)/wininet.dll

.PHONY: all clean distclean deps

all: $(TARGET)

$(TARGET): $(OBJS) $(MONGOOSE_OBJ) $(DETOURS_LIB) | $(BUILD_DIR)
	$(CXX) -o $@ $(OBJS) $(MONGOOSE_OBJ) $(DETOURS_LIB) $(LDFLAGS)
	$(STRIP) $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(MONGOOSE_OBJ): $(SRC_DIR)/mongoose.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(MONGOOSE_DEFS) -c $< -o $@

DETOURS_CXXFLAGS := -std=c++14 -Os -Wall -fno-rtti \
                    -ffunction-sections -fdata-sections \
                    $(COMMON_DEFS)

$(BUILD_DIR)/detours/%.o: $(DEPS_DIR)/Detours/src/%.cpp | $(BUILD_DIR)/detours
	$(CXX) $(DETOURS_CXXFLAGS) -I$(DEPS_DIR)/Detours/src -DDETOURS_X86 -c $< -o $@

$(DETOURS_LIB): $(DETOURS_OBJ)
	$(AR) rcs $@ $^

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/detours:
	mkdir -p $(BUILD_DIR)/detours

deps:
	mkdir -p $(DEPS_DIR)
	[ -d $(DEPS_DIR)/Detours ] || git clone --depth 1 https://github.com/microsoft/Detours.git $(DEPS_DIR)/Detours

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf $(DEPS_DIR)
