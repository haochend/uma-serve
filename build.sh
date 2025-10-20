#!/bin/bash
#
# This is a simple build script for the UMA Serve project.
# It automates the CMake configuration and build steps.
#
# Usage:
#   ./build.sh        # Configure and build in Debug mode
#   ./build.sh release  # Configure and build in Release mode
#

# Stop the script if any command fails
set -e

# 1. Set the build type
# Default to Debug if no argument is given
BUILD_TYPE="Debug"
if [[ "$1" == "release" ]]; then
  BUILD_TYPE="Release"
fi
echo "--- Building in $BUILD_TYPE mode ---"

# 2. Define our build directory
BUILD_DIR="build"

# 3. Configure the project using CMake
#    -S .         : The source directory is here ('.')
#    -B $BUILD_DIR: The build directory is ./build
#    -D...        : Pass a variable to CMake
cmake -S . -B $BUILD_DIR -DCMAKE_BUILD_TYPE=$BUILD_TYPE

# 4. Build the project
#    --build $BUILD_DIR: Tell CMake to run the build process
#                        in the 'build' directory.
cmake --build $BUILD_DIR

# 5. (Optional) A nice message on success
echo "--- Build complete! ---"
echo "Your executable is at: $BUILD_DIR/umad"
