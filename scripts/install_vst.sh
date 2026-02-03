#!/bin/bash
# Script d'installation pour Sp3ctra VST (depuis binaires pré-compilés)
# Usage: ./scripts/install_vst.sh [vst3|au|standalone|all]

set -e  # Stop on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Project paths
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREBUILT_DIR="$PROJECT_DIR/prebuilt"

# Installation paths
VST3_INSTALL_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_INSTALL_DIR="/Library/Audio/Plug-Ins/Components"

# Source paths (prebuilt ZIP archives)
VST3_ARCHIVE="$PREBUILT_DIR/Sp3ctra-VST3.zip"
AU_ARCHIVE="$PREBUILT_DIR/Sp3ctra-AU.zip"
STANDALONE_ARCHIVE="$PREBUILT_DIR/Sp3ctra-Standalone.zip"

# Temporary extraction directory
TEMP_DIR="$PREBUILT_DIR/.temp"

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Sp3ctra VST Installation Script${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Parse arguments (default: install all)
INSTALL_VST3=0
INSTALL_AU=0
INSTALL_STANDALONE=0

if [ $# -eq 0 ]; then
    # No arguments: install all
    INSTALL_VST3=1
    INSTALL_AU=1
    INSTALL_STANDALONE=0  # Standalone is not "installed", just available
else
    # Parse specific formats
    for arg in "$@"; do
        case "$arg" in
            vst3|VST3)
                INSTALL_VST3=1
                ;;
            au|AU)
                INSTALL_AU=1
                ;;
            standalone|Standalone)
                INSTALL_STANDALONE=1
                ;;
            all)
                INSTALL_VST3=1
                INSTALL_AU=1
                INSTALL_STANDALONE=0
                ;;
            help|--help|-h)
                echo "Usage: $0 [vst3] [au] [standalone] [all] [help]"
                echo ""
                echo "Options:"
                echo "  vst3       - Install VST3 plugin only"
                echo "  au         - Install Audio Unit plugin only"
                echo "  standalone - Show standalone location (not installed)"
                echo "  all        - Install VST3 and AU (default)"
                echo "  help       - Show this help message"
                echo ""
                echo "Examples:"
                echo "  $0              # Install VST3 and AU"
                echo "  $0 vst3         # Install VST3 only"
                echo "  $0 au           # Install AU only"
                echo "  $0 standalone   # Show standalone app location"
                echo ""
                exit 0
                ;;
            *)
                echo -e "${RED}Unknown option: $arg${NC}"
                echo "Use '$0 help' for usage information"
                exit 1
                ;;
        esac
    done
fi

# Check if prebuilt directory exists
if [ ! -d "$PREBUILT_DIR" ]; then
    echo -e "${RED}✗ Prebuilt directory not found: $PREBUILT_DIR${NC}"
    echo -e "${YELLOW}Please compile the project first using:${NC}"
    echo "  ./scripts/build_vst.sh"
    exit 1
fi

# Function to extract and install a plugin from ZIP archive
install_plugin_from_zip() {
    local ARCHIVE="$1"
    local DEST_DIR="$2"
    local PLUGIN_NAME="$3"
    local NEEDS_SUDO="$4"
    
    if [ ! -f "$ARCHIVE" ]; then
        echo -e "${YELLOW}⚠ $PLUGIN_NAME archive not found${NC}"
        echo -e "${YELLOW}  Expected: $ARCHIVE${NC}"
        echo -e "${YELLOW}  Please compile the project first: ./scripts/build_vst.sh${NC}"
        return 1
    fi
    
    echo -e "${BLUE}Installing $PLUGIN_NAME from ZIP archive...${NC}"
    
    # Create temporary directory for extraction
    mkdir -p "$TEMP_DIR"
    
    # Extract archive
    echo -e "${YELLOW}  Extracting archive...${NC}"
    unzip -q -o "$ARCHIVE" -d "$TEMP_DIR"
    
    # Find the extracted bundle
    local BUNDLE_NAME=""
    if [ "$PLUGIN_NAME" = "VST3" ]; then
        BUNDLE_NAME="Sp3ctra.vst3"
    elif [ "$PLUGIN_NAME" = "AU" ]; then
        BUNDLE_NAME="Sp3ctra.component"
    elif [ "$PLUGIN_NAME" = "Standalone" ]; then
        BUNDLE_NAME="Sp3ctra.app"
    fi
    
    local EXTRACTED_BUNDLE="$TEMP_DIR/$BUNDLE_NAME"
    
    if [ ! -e "$EXTRACTED_BUNDLE" ]; then
        echo -e "${RED}✗ Failed to extract $BUNDLE_NAME${NC}"
        rm -rf "$TEMP_DIR"
        return 1
    fi
    
    echo -e "${YELLOW}  Installing to $DEST_DIR/${NC}"
    
    # Create destination directory
    if [ "$NEEDS_SUDO" = "true" ]; then
        sudo mkdir -p "$DEST_DIR"
    else
        mkdir -p "$DEST_DIR"
    fi
    
    # Remove existing installation if present
    local DEST_PATH="$DEST_DIR/$BUNDLE_NAME"
    if [ -e "$DEST_PATH" ]; then
        echo -e "${YELLOW}  Removing existing installation...${NC}"
        if [ "$NEEDS_SUDO" = "true" ]; then
            sudo rm -rf "$DEST_PATH"
        else
            rm -rf "$DEST_PATH"
        fi
    fi
    
    # Copy plugin
    if [ "$NEEDS_SUDO" = "true" ]; then
        sudo cp -R "$EXTRACTED_BUNDLE" "$DEST_DIR/"
    else
        cp -R "$EXTRACTED_BUNDLE" "$DEST_DIR/"
    fi
    
    # Cleanup temp directory
    rm -rf "$TEMP_DIR"
    
    echo -e "${GREEN}✓ $PLUGIN_NAME installed successfully${NC}"
    return 0
}

# Install VST3
if [ $INSTALL_VST3 -eq 1 ]; then
    echo ""
    install_plugin_from_zip "$VST3_ARCHIVE" "$VST3_INSTALL_DIR" "VST3" "false"
fi

# Install AU
if [ $INSTALL_AU -eq 1 ]; then
    echo ""
    echo -e "${YELLOW}Audio Unit installation requires administrator privileges${NC}"
    install_plugin_from_zip "$AU_ARCHIVE" "$AU_INSTALL_DIR" "AU" "true"
fi

# Show standalone location
if [ $INSTALL_STANDALONE -eq 1 ]; then
    echo ""
    if [ -f "$STANDALONE_ARCHIVE" ]; then
        # Extract standalone to temp and show location
        mkdir -p "$TEMP_DIR"
        unzip -q -o "$STANDALONE_ARCHIVE" -d "$TEMP_DIR"
        STANDALONE_APP="$TEMP_DIR/Sp3ctra.app"
        
        if [ -e "$STANDALONE_APP" ]; then
            echo -e "${GREEN}✓ Standalone app extracted${NC}"
            echo -e "${BLUE}To launch standalone:${NC}"
            echo "  open \"$STANDALONE_APP\""
            echo ""
            echo -e "${YELLOW}Note: Standalone is temporary. For permanent access, extract manually:${NC}"
            echo "  unzip \"$STANDALONE_ARCHIVE\" -d ~/Applications/"
        else
            echo -e "${RED}✗ Failed to extract Standalone${NC}"
            rm -rf "$TEMP_DIR"
        fi
    else
        echo -e "${YELLOW}⚠ Standalone archive not found in prebuilt directory${NC}"
    fi
fi

# Summary
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Installation Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

if [ $INSTALL_VST3 -eq 1 ] || [ $INSTALL_AU -eq 1 ]; then
    echo -e "${BLUE}Next steps:${NC}"
    echo "  1. Rescan plugins in your DAW"
    echo "  2. Look for 'Sp3ctra' by Ondulab"
    echo "  3. Load it as an instrument"
    echo ""
fi

# List installed locations
echo -e "${BLUE}Installed locations:${NC}"
if [ $INSTALL_VST3 -eq 1 ] && [ -e "$VST3_INSTALL_DIR/Sp3ctra.vst3" ]; then
    echo -e "${GREEN}  ✓ VST3:${NC} $VST3_INSTALL_DIR/Sp3ctra.vst3"
fi
if [ $INSTALL_AU -eq 1 ] && [ -e "$AU_INSTALL_DIR/Sp3ctra.component" ]; then
    echo -e "${GREEN}  ✓ AU:${NC} $AU_INSTALL_DIR/Sp3ctra.component"
fi

echo ""
