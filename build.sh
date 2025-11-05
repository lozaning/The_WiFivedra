#!/bin/bash
# Build script for WiFivedra firmware

set -e

# Create src directory if it doesn't exist
mkdir -p src

if [ "$1" == "controller" ]; then
    echo "Building controller firmware..."
    cp controller/controller.ino src/main.cpp
    pio run -e controller
    echo "Controller firmware built successfully!"
    echo "Binary: .pio/build/controller/firmware.bin"
elif [ "$1" == "subordinate" ]; then
    echo "Building subordinate firmware..."
    cp subordinate/subordinate.ino src/main.cpp
    pio run -e subordinate
    echo "Subordinate firmware built successfully!"
    echo "Binary: .pio/build/subordinate/firmware.bin"
elif [ "$1" == "all" ]; then
    echo "Building all firmware..."

    echo "Building controller..."
    cp controller/controller.ino src/main.cpp
    pio run -e controller

    echo "Building subordinate..."
    cp subordinate/subordinate.ino src/main.cpp
    pio run -e subordinate

    echo "All firmware built successfully!"
    echo "Controller binary: .pio/build/controller/firmware.bin"
    echo "Subordinate binary: .pio/build/subordinate/firmware.bin"
elif [ "$1" == "clean" ]; then
    echo "Cleaning build files..."
    rm -rf .pio src
    echo "Clean complete!"
else
    echo "Usage: $0 {controller|subordinate|all|clean}"
    echo ""
    echo "Examples:"
    echo "  $0 controller   - Build controller firmware"
    echo "  $0 subordinate  - Build subordinate firmware"
    echo "  $0 all          - Build both firmwares"
    echo "  $0 clean        - Clean build files"
    exit 1
fi
