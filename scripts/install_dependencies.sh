#!/bin/bash

################################################################################
# Sp3ctra VST - Dependency Installation Script
# 
# This script installs all required dependencies for building Sp3ctra VST
# Supports: macOS, Linux (Debian/Ubuntu-based)
#
# Usage: bash scripts/install_dependencies.sh
################################################################################

set -e  # Exit on error

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Detect OS
OS="unknown"
if [[ "$OSTYPE" == "darwin"* ]]; then
    OS="macos"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    OS="linux"
fi

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Sp3ctra VST - Dependency Installation${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo -e "Detected OS: ${GREEN}$OS${NC}"
echo ""

################################################################################
# Function: Check if command exists
################################################################################
command_exists() {
    command -v "$1" &> /dev/null
}

################################################################################
# Function: Check compiler version
################################################################################
check_compiler() {
    echo -e "${BLUE}Checking C++ compiler...${NC}"
    
    if command_exists clang++; then
        COMPILER_VERSION=$(clang++ --version | head -n1)
        echo -e "${GREEN}✓${NC} Found: $COMPILER_VERSION"
        return 0
    elif command_exists g++; then
        COMPILER_VERSION=$(g++ --version | head -n1)
        echo -e "${GREEN}✓${NC} Found: $COMPILER_VERSION"
        return 0
    else
        echo -e "${RED}✗${NC} No C++ compiler found!"
        return 1
    fi
}

################################################################################
# Function: Check CMake version
################################################################################
check_cmake() {
    echo -e "${BLUE}Checking CMake...${NC}"
    
    if command_exists cmake; then
        CMAKE_VERSION=$(cmake --version | head -n1 | awk '{print $3}')
        REQUIRED_VERSION="3.15"
        
        # Simple version comparison
        if [ "$(printf '%s\n' "$REQUIRED_VERSION" "$CMAKE_VERSION" | sort -V | head -n1)" = "$REQUIRED_VERSION" ]; then
            echo -e "${GREEN}✓${NC} Found CMake $CMAKE_VERSION (>= $REQUIRED_VERSION)"
            return 0
        else
            echo -e "${YELLOW}⚠${NC} CMake $CMAKE_VERSION found, but version >= $REQUIRED_VERSION required"
            return 1
        fi
    else
        echo -e "${RED}✗${NC} CMake not found!"
        return 1
    fi
}

################################################################################
# macOS Installation
################################################################################
install_macos_dependencies() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}Installing macOS dependencies${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
    
    # Step 1: Check and install Homebrew
    if ! command_exists brew; then
        echo -e "${YELLOW}Homebrew not found. Installing...${NC}"
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        
        # Add Homebrew to PATH for Apple Silicon Macs
        if [[ $(uname -m) == "arm64" ]]; then
            echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile
            eval "$(/opt/homebrew/bin/brew shellenv)"
        fi
    else
        echo -e "${GREEN}✓${NC} Homebrew already installed"
    fi
    
    # Step 2: Check and install Xcode Command Line Tools
    echo ""
    echo -e "${BLUE}Checking Xcode Command Line Tools...${NC}"
    
    if ! xcode-select -p &> /dev/null; then
        echo -e "${YELLOW}Xcode Command Line Tools not found. Installing...${NC}"
        echo -e "${YELLOW}A dialog will appear - please follow the installation prompts.${NC}"
        xcode-select --install
        
        echo ""
        echo -e "${YELLOW}⚠ Please wait for Xcode Command Line Tools installation to complete.${NC}"
        echo -e "${YELLOW}Press ENTER when installation is finished...${NC}"
        read -r
    else
        echo -e "${GREEN}✓${NC} Xcode Command Line Tools already installed"
        
        # Verify compiler is accessible
        if ! check_compiler; then
            echo -e "${YELLOW}Compiler not accessible. Resetting Xcode tools...${NC}"
            sudo xcode-select --reset
            check_compiler || {
                echo -e "${RED}ERROR: Compiler still not accessible after reset.${NC}"
                echo -e "${RED}Please run: sudo xcode-select --install${NC}"
                exit 1
            }
        fi
    fi
    
    # Step 3: Install/update CMake
    echo ""
    if ! check_cmake; then
        echo -e "${YELLOW}Installing/updating CMake...${NC}"
        brew install cmake
    fi
    
    # Step 4: Install audio libraries (for standalone mode)
    echo ""
    echo -e "${BLUE}Installing audio libraries (optional, for standalone)...${NC}"
    
    AUDIO_LIBS=("fftw" "libsndfile" "rtaudio" "rtmidi")
    
    for lib in "${AUDIO_LIBS[@]}"; do
        if brew list "$lib" &> /dev/null; then
            echo -e "${GREEN}✓${NC} $lib already installed"
        else
            echo -e "${YELLOW}Installing $lib...${NC}"
            brew install "$lib" || echo -e "${YELLOW}⚠ Failed to install $lib (non-critical for VST)${NC}"
        fi
    done
    
    # Step 5: Git (should be installed with Xcode tools, but verify)
    echo ""
    if ! command_exists git; then
        echo -e "${YELLOW}Installing Git...${NC}"
        brew install git
    else
        echo -e "${GREEN}✓${NC} Git already installed"
    fi
}

################################################################################
# Linux Installation (Debian/Ubuntu)
################################################################################
install_linux_dependencies() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}Installing Linux dependencies${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
    
    # Update package list
    echo -e "${BLUE}Updating package list...${NC}"
    sudo apt update
    
    # Step 1: Install build essentials
    echo ""
    echo -e "${BLUE}Installing build tools...${NC}"
    
    BUILD_PACKAGES=(
        "build-essential"
        "cmake"
        "git"
        "curl"
        "pkg-config"
    )
    
    for package in "${BUILD_PACKAGES[@]}"; do
        if dpkg -l | grep -q "^ii  $package "; then
            echo -e "${GREEN}✓${NC} $package already installed"
        else
            echo -e "${YELLOW}Installing $package...${NC}"
            sudo apt install -y "$package"
        fi
    done
    
    # Step 2: Verify compiler
    echo ""
    check_compiler || {
        echo -e "${RED}ERROR: Compiler not found after build-essential installation!${NC}"
        exit 1
    }
    
    # Step 3: Verify CMake
    echo ""
    check_cmake || {
        echo -e "${YELLOW}CMake version too old. Installing from Kitware repository...${NC}"
        
        # Add Kitware APT repository for latest CMake
        wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
        echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ focal main' | sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null
        sudo apt update
        sudo apt install -y cmake
        
        check_cmake || {
            echo -e "${RED}ERROR: Failed to install CMake >= 3.15${NC}"
            exit 1
        }
    }
    
    # Step 4: Install audio development libraries (for standalone)
    echo ""
    echo -e "${BLUE}Installing audio libraries (optional, for standalone)...${NC}"
    
    AUDIO_PACKAGES=(
        "libfftw3-dev"
        "libsndfile1-dev"
        "libasound2-dev"
        "librtaudio-dev"
        "librtmidi-dev"
        "libjack-jackd2-dev"
        "libfreetype6-dev"
        "libx11-dev"
        "libxext-dev"
        "libxrandr-dev"
        "libxinerama-dev"
        "libxcursor-dev"
    )
    
    for package in "${AUDIO_PACKAGES[@]}"; do
        if dpkg -l | grep -q "^ii  $package "; then
            echo -e "${GREEN}✓${NC} $package already installed"
        else
            echo -e "${YELLOW}Installing $package...${NC}"
            sudo apt install -y "$package" || echo -e "${YELLOW}⚠ Failed to install $package (non-critical for VST)${NC}"
        fi
    done
}

################################################################################
# Main Installation
################################################################################

case "$OS" in
    macos)
        install_macos_dependencies
        ;;
    linux)
        install_linux_dependencies
        ;;
    *)
        echo -e "${RED}ERROR: Unsupported operating system: $OSTYPE${NC}"
        echo "This script supports macOS and Linux (Debian/Ubuntu-based) only."
        exit 1
        ;;
esac

################################################################################
# Final Verification
################################################################################

echo ""
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Final Verification${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Verify essential tools
VERIFICATION_PASSED=true

echo -e "${BLUE}Checking essential tools:${NC}"
for tool in cmake git; do
    if command_exists "$tool"; then
        VERSION=$($tool --version | head -n1)
        echo -e "${GREEN}✓${NC} $tool: $VERSION"
    else
        echo -e "${RED}✗${NC} $tool: NOT FOUND"
        VERIFICATION_PASSED=false
    fi
done

# Verify compiler
echo ""
if check_compiler; then
    echo ""
else
    VERIFICATION_PASSED=false
fi

# Check CMake version
if check_cmake; then
    echo ""
else
    VERIFICATION_PASSED=false
fi

################################################################################
# Summary
################################################################################

echo ""
echo -e "${BLUE}========================================${NC}"
if [ "$VERIFICATION_PASSED" = true ]; then
    echo -e "${GREEN}✓ Installation completed successfully!${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
    echo -e "${GREEN}Next steps:${NC}"
    echo "1. Build the VST plugin:"
    echo -e "   ${YELLOW}bash scripts/build_vst.sh${NC}"
    echo ""
    echo "2. Or build manually:"
    echo -e "   ${YELLOW}cd vst && mkdir -p build && cd build${NC}"
    echo -e "   ${YELLOW}cmake ..${NC}"
    echo -e "   ${YELLOW}cmake --build .${NC}"
    echo ""
    echo -e "${BLUE}Note:${NC} JUCE framework will be downloaded automatically by CMake during build."
else
    echo -e "${RED}✗ Installation completed with errors${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
    echo -e "${RED}Please fix the errors above and run this script again.${NC}"
    echo ""
    echo -e "${YELLOW}Common troubleshooting:${NC}"
    
    if [ "$OS" = "macos" ]; then
        echo "- If compiler not found, run: sudo xcode-select --install"
        echo "- If Xcode license agreement needed: sudo xcodebuild -license accept"
        echo "- Reset Xcode tools: sudo xcode-select --reset"
    else
        echo "- Ensure your system is up to date: sudo apt update && sudo apt upgrade"
        echo "- Verify build-essential: sudo apt install --reinstall build-essential"
    fi
    
    exit 1
fi

echo -e "${BLUE}========================================${NC}"
