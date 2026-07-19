#!/bin/bash
# Script de build pour Sp3ctra VST
# Usage: ./scripts/build_vst.sh [clean|install|help]

set -e  # Stop on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Project paths
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VST_DIR="$PROJECT_DIR/vst"
BUILD_DIR="$VST_DIR/build"

# Installation paths
VST3_INSTALL_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_INSTALL_DIR="/Library/Audio/Plug-Ins/Components"

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Sp3ctra VST Build Script${NC}"
echo -e "${BLUE}========================================${NC}"

# Parse command line arguments
CLEAN_BUILD=0
INSTALL_PLUGINS=0
RUN_STANDALONE=0
FULL_BUILD=0
BUILD_CONFIG="Release"

# Parse all arguments
for arg in "$@"; do
    case "$arg" in
        clean|--clean)
            CLEAN_BUILD=1
            echo -e "${YELLOW}Clean build requested${NC}"
            ;;
        install|--install)
            INSTALL_PLUGINS=1
            echo -e "${YELLOW}Installation requested${NC}"
            ;;
        debug|--debug)
            BUILD_CONFIG="Debug"
            echo -e "${YELLOW}Debug build requested${NC}"
            ;;
        run|--run)
            RUN_STANDALONE=1
            echo -e "${YELLOW}Will launch standalone after build${NC}"
            ;;
        all|--all)
            FULL_BUILD=1
            echo -e "${YELLOW}Full build requested (all formats + archives)${NC}"
            ;;
        help|--help|-h)
            echo ""
            echo "Usage: $0 [clean] [install] [debug] [run] [all] [help]"
            echo ""
            echo "Options:"
            echo "  clean    - Clean build directory before building"
            echo "  install  - Install plugins after successful build"
            echo "  debug    - Build in Debug mode (default: Release)"
            echo "  run      - Launch standalone after successful build"
            echo "           (run alone = fast build: Standalone only, no ZIPs;"
            echo "            add 'all' or 'install' to build every format)"
            echo "  all      - Force building all formats + ZIP archives"
            echo "  help     - Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                    # Standard Release build (all formats + ZIPs)"
            echo "  $0 clean              # Clean + Release build"
            echo "  $0 install            # Build + Install to system"
            echo "  $0 run                # FAST: Standalone-only build + Launch"
            echo "  $0 run all            # Full build + Launch"
            echo "  $0 debug run          # Debug Standalone build + Launch"
            echo "  $0 clean debug run    # Clean + Debug + Launch"
            echo ""
            exit 0
            ;;
    esac
done

# Fast mode: 'run' without 'install'/'all' builds only the Standalone target
# and skips ZIP archiving — VST3/AU compile+link and 3x200MB zips are the bulk
# of an incremental iteration.
FAST_BUILD=0
if [ $RUN_STANDALONE -eq 1 ] && [ $INSTALL_PLUGINS -eq 0 ] && [ $FULL_BUILD -eq 0 ]; then
    FAST_BUILD=1
    echo -e "${YELLOW}Fast mode: Standalone only (use 'all' for VST3/AU + ZIPs)${NC}"
fi

# Clean build directory if requested
if [ $CLEAN_BUILD -eq 1 ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"/*
        echo -e "${GREEN}✓ Build directory cleaned${NC}"
    fi
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Compiler cache (big speedup on clean rebuilds / branch or config switches)
CMAKE_EXTRA_ARGS=()
if command -v ccache >/dev/null 2>&1; then
    CMAKE_EXTRA_ARGS+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# Skip the configure step (~3s) when the cache already matches; a CMakeLists
# change still triggers auto-reconfigure from `cmake --build`.
NEED_CONFIGURE=1
if [ -f "$BUILD_DIR/CMakeCache.txt" ] \
   && grep -q "^CMAKE_BUILD_TYPE:STRING=$BUILD_CONFIG\$" "$BUILD_DIR/CMakeCache.txt"; then
    if [ ${#CMAKE_EXTRA_ARGS[@]} -eq 0 ] \
       || grep -q "^CMAKE_CXX_COMPILER_LAUNCHER:.*=ccache\$" "$BUILD_DIR/CMakeCache.txt"; then
        NEED_CONFIGURE=0
    fi
fi

if [ $NEED_CONFIGURE -eq 1 ]; then
    echo ""
    echo -e "${BLUE}Configuring project with CMake ($BUILD_CONFIG)...${NC}"
    if cmake -DCMAKE_BUILD_TYPE=$BUILD_CONFIG "${CMAKE_EXTRA_ARGS[@]}" ..; then
        echo -e "${GREEN}✓ Configuration successful (CMAKE_BUILD_TYPE=$BUILD_CONFIG)${NC}"
    else
        echo -e "${RED}✗ Configuration failed${NC}"
        exit 1
    fi
else
    echo ""
    echo -e "${GREEN}✓ CMake cache up to date ($BUILD_CONFIG) — configure skipped${NC}"
fi

# Build
echo ""
echo -e "${BLUE}Building project ($BUILD_CONFIG)...${NC}"
echo -e "${YELLOW}This may take 2-3 minutes on first build...${NC}"

BUILD_TARGET_ARGS=()
if [ $FAST_BUILD -eq 1 ]; then
    BUILD_TARGET_ARGS=(--target Sp3ctraVST_Standalone)
fi

if cmake --build . --config $BUILD_CONFIG "${BUILD_TARGET_ARGS[@]}" -j$(sysctl -n hw.ncpu); then
    echo -e "${GREEN}✓ Build successful${NC}"
else
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi

# Check if artifacts were created (CMake places them in Release/Debug subfolder)
VST3_PATH="$BUILD_DIR/Sp3ctraVST_artefacts/$BUILD_CONFIG/VST3/Sp3ctra.vst3"
AU_PATH="$BUILD_DIR/Sp3ctraVST_artefacts/$BUILD_CONFIG/AU/Sp3ctra.component"
STANDALONE_PATH="$BUILD_DIR/Sp3ctraVST_artefacts/$BUILD_CONFIG/Standalone/Sp3ctra.app"

echo ""
echo -e "${BLUE}Build artifacts:${NC}"

if [ $FAST_BUILD -eq 0 ]; then
    if [ -d "$VST3_PATH" ]; then
        echo -e "${GREEN}✓ VST3:${NC} $VST3_PATH"
    else
        echo -e "${YELLOW}⚠ VST3 not found${NC}"
    fi

    if [ -d "$AU_PATH" ]; then
        echo -e "${GREEN}✓ AU:${NC} $AU_PATH"
    else
        echo -e "${YELLOW}⚠ AU not found${NC}"
    fi
fi

if [ -d "$STANDALONE_PATH" ]; then
    echo -e "${GREEN}✓ Standalone:${NC} $STANDALONE_PATH"
else
    echo -e "${YELLOW}⚠ Standalone not found${NC}"
fi

# Create ZIP archives for Git distribution (solves symlink issues).
# Skipped in fast mode: zipping 3x200MB bundles dominates iteration time.
if [ "$BUILD_CONFIG" = "Release" ] && [ $FAST_BUILD -eq 0 ]; then
    echo ""
    echo -e "${BLUE}Creating ZIP archives for Git distribution...${NC}"
    
    PREBUILT_DIR="$PROJECT_DIR/prebuilt"
    mkdir -p "$PREBUILT_DIR"
    
    # Create VST3 archive
    if [ -d "$VST3_PATH" ]; then
        echo -e "${YELLOW}Creating VST3 archive...${NC}"
        cd "$(dirname "$VST3_PATH")"
        rm -f "$PREBUILT_DIR/Sp3ctra-VST3.zip"
        zip -r -q "$PREBUILT_DIR/Sp3ctra-VST3.zip" "Sp3ctra.vst3"
        VST3_SIZE=$(du -h "$PREBUILT_DIR/Sp3ctra-VST3.zip" | cut -f1)
        echo -e "${GREEN}✓ VST3 archive created: $VST3_SIZE${NC}"
        cd "$BUILD_DIR"
    fi
    
    # Create AU archive
    if [ -d "$AU_PATH" ]; then
        echo -e "${YELLOW}Creating AU archive...${NC}"
        cd "$(dirname "$AU_PATH")"
        rm -f "$PREBUILT_DIR/Sp3ctra-AU.zip"
        zip -r -q "$PREBUILT_DIR/Sp3ctra-AU.zip" "Sp3ctra.component"
        AU_SIZE=$(du -h "$PREBUILT_DIR/Sp3ctra-AU.zip" | cut -f1)
        echo -e "${GREEN}✓ AU archive created: $AU_SIZE${NC}"
        cd "$BUILD_DIR"
    fi
    
    # Create Standalone archive
    if [ -d "$STANDALONE_PATH" ]; then
        echo -e "${YELLOW}Creating Standalone archive...${NC}"
        cd "$(dirname "$STANDALONE_PATH")"
        rm -f "$PREBUILT_DIR/Sp3ctra-Standalone.zip"
        zip -r -q "$PREBUILT_DIR/Sp3ctra-Standalone.zip" "Sp3ctra.app"
        STANDALONE_SIZE=$(du -h "$PREBUILT_DIR/Sp3ctra-Standalone.zip" | cut -f1)
        echo -e "${GREEN}✓ Standalone archive created: $STANDALONE_SIZE${NC}"
        cd "$BUILD_DIR"
    fi
    
    echo ""
    echo -e "${GREEN}✓ ZIP archives ready for Git distribution in prebuilt/${NC}"
    echo -e "${BLUE}Archives can now be committed to Git:${NC}"
    echo "  git add prebuilt/*.zip"
    echo "  git commit -m \"chore: update prebuilt binaries\""
fi

# Install if requested
if [ $INSTALL_PLUGINS -eq 1 ]; then
    echo ""
    echo -e "${BLUE}Installing plugins...${NC}"
    
    # Install VST3
    if [ -d "$VST3_PATH" ]; then
        mkdir -p "$VST3_INSTALL_DIR"
        echo -e "${YELLOW}Installing VST3 to $VST3_INSTALL_DIR${NC}"
        cp -r "$VST3_PATH" "$VST3_INSTALL_DIR/"
        echo -e "${GREEN}✓ VST3 installed${NC}"
    fi
    
    # Install AU (requires sudo)
    if [ -d "$AU_PATH" ]; then
        echo -e "${YELLOW}Installing AU to $AU_INSTALL_DIR (requires sudo)${NC}"
        sudo cp -r "$AU_PATH" "$AU_INSTALL_DIR/"
        echo -e "${GREEN}✓ AU installed${NC}"
    fi
    
    echo ""
    echo -e "${GREEN}Installation complete!${NC}"
    echo -e "${BLUE}Next steps:${NC}"
    echo "  1. Rescan plugins in your DAW"
    echo "  2. Look for 'Sp3ctra' in your instrument list"
    echo "  3. Load it and you should hear a 440Hz test tone"
fi

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Build Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Display next steps if not installing
if [ $INSTALL_PLUGINS -eq 0 ] && [ $RUN_STANDALONE -eq 0 ]; then
    echo -e "${BLUE}To install plugins, run:${NC}"
    echo "  $0 install"
    echo ""
    echo -e "${BLUE}To run standalone, execute:${NC}"
    echo "  ./scripts/run_standalone.sh"
    echo ""
    echo -e "${BLUE}Or manually copy:${NC}"
    echo "  VST3: cp -r \"$VST3_PATH\" \"$VST3_INSTALL_DIR/\""
    echo "  AU:   sudo cp -r \"$AU_PATH\" \"$AU_INSTALL_DIR/\""
    echo ""
fi

# Launch standalone if requested
if [ $RUN_STANDALONE -eq 1 ]; then
    echo ""
    echo -e "${BLUE}Launching standalone...${NC}"
    if [ -d "$STANDALONE_PATH" ]; then
        STANDALONE_EXEC="$STANDALONE_PATH/Contents/MacOS/Sp3ctra"
        if [ -f "$STANDALONE_EXEC" ]; then
            echo -e "${GREEN}Starting Sp3ctra standalone ($BUILD_CONFIG)...${NC}"
            echo -e "${YELLOW}Press Ctrl+C to stop${NC}"
            echo ""
            "$STANDALONE_EXEC"
        else
            echo -e "${RED}✗ Standalone executable not found${NC}"
            exit 1
        fi
    else
        echo -e "${RED}✗ Standalone app not found${NC}"
        exit 1
    fi
fi
