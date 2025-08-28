# Makefile for Musketeer-Stockfish Betza Integration
# Based on Fairy-Stockfish build procedures

# Architecture and compiler settings (following Fairy-Stockfish approach)
ARCH = x86-64-modern
COMP = mingw
debug = no
optimize = yes
profile_use = no

# Compiler
ifeq ($(COMP),mingw)
        CXX = g++
        LDFLAGS += -static -static-libgcc -static-libstdc++
endif

# Base compiler flags
CXXFLAGS = -std=c++17 -Wall -Wcast-qual -fno-exceptions $(EXTRACXXFLAGS)
LDFLAGS = $(EXTRALDFLAGS)

# Optimization flags
ifeq ($(optimize),yes)
        CXXFLAGS += -O3 -flto=auto
        LDFLAGS += -flto=auto
endif

# Debug flags
ifeq ($(debug),no)
        CXXFLAGS += -DNDEBUG
else
        CXXFLAGS += -g
endif

# Architecture-specific flags
ifeq ($(ARCH),x86-64-modern)
        CXXFLAGS += -msse3 -mpopcnt
        prefetch = yes
endif

# Enable prefetch
ifeq ($(prefetch),yes)
        CXXFLAGS += -DUSE_PREFETCH
endif

# Source files - include ALL cpp files from src directory
SOURCES = \
        src/benchmark.cpp \
        src/betza.cpp \
        src/bitbase.cpp \
        src/bitboard.cpp \
        src/endgame.cpp \
        src/evaluate.cpp \
        src/main.cpp \
        src/material.cpp \
        src/misc.cpp \
        src/movegen.cpp \
        src/movepick.cpp \
        src/pawns.cpp \
        src/position.cpp \
        src/psqt.cpp \
        src/search.cpp \
        src/syzygy/tbprobe.cpp \
        src/thread.cpp \
        src/timeman.cpp \
        src/tt.cpp \
        src/uci.cpp \
        src/ucioption.cpp \
        src/xboard.cpp

# Object files
OBJDIR = obj
OBJECTS = $(SOURCES:src/%.cpp=$(OBJDIR)/%.o)

# Target executables
TARGET1 = stockfish-betza-windows.exe
TARGET2 = stockfish-betza-bmi2-windows.exe

.PHONY: all clean build

all: build

build: $(TARGET1) $(TARGET2)

# Create object directory
$(OBJDIR):
        @mkdir -p $(OBJDIR)
        @mkdir -p $(OBJDIR)/syzygy

# Compile object files
$(OBJDIR)/%.o: src/%.cpp | $(OBJDIR)
        $(CXX) $(CXXFLAGS) -c $< -o $@

# Build standard executable
$(TARGET1): $(OBJECTS)
        $(CXX) -o $@ $^ $(LDFLAGS)

# Build BMI2 executable (with additional BMI2 instructions)
$(TARGET2): $(SOURCES) | $(OBJDIR)
        $(CXX) $(CXXFLAGS) -DUSE_BMI2 -mbmi -mbmi2 -o $@ $(SOURCES) $(LDFLAGS)

# Support building specific target by TARGET variable
ifneq ($(TARGET),)
$(TARGET): $(SOURCES) | $(OBJDIR)
        $(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS)
endif

clean:
        rm -rf $(OBJDIR) $(TARGET1) $(TARGET2)

# Help target
help:
        @echo "Available targets:"
        @echo "  build      - Build both executables (default)"
        @echo "  clean      - Remove all generated files"
        @echo "  help       - Show this help"
        @echo ""
        @echo "Configuration:"
        @echo "  ARCH=$(ARCH)"
        @echo "  COMP=$(COMP)"
        @echo "  debug=$(debug)"
        @echo "  optimize=$(optimize)"