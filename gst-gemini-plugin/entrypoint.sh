#!/bin/sh
set -e

# --- Colors ---
RESET="\033[0m"
BOLD="\033[1m"
GREEN="\033[0;32m"
YELLOW="\033[0;33m"
CYAN="\033[0;36m"
RED="\033[0;31m"
# --- End Colors ---

# Default action if nothing is passed to 'docker run ... image <action>'
ACTION=${1:-build}
EXAMPLE_FILE_TO_TEST=${2} # Second argument for test-examples

# Base directory where the main project source code is mounted
APP_DIR="/builder"
# Directory where examples are expected to be mounted
EXAMPLES_MOUNT_DIR="/examples"
# Output directory for compiled C examples, relative to EXAMPLES_MOUNT_DIR
EXAMPLES_OUTPUT_DIR="bin" # This will be $EXAMPLES_MOUNT_DIR/bin

# --- Helper Function to Build and/or Install Main Plugin ---
ensure_main_plugin_built_and_installed() {
    ACTION_REQUIRING_PLUGIN_INSTALL=$1 # "build-examples" or "test-examples"

    echo "${CYAN}--- Ensuring main plugin from '$APP_DIR' is built and installed (for $ACTION_REQUIRING_PLUGIN_INSTALL) ---${RESET}"
    if [ ! -d "$APP_DIR" ]; then
      echo "${RED}${BOLD}Error: Main project directory '$APP_DIR' not found or not mounted. Cannot proceed.${RESET}"
      exit 1
    fi

    if ! cd "$APP_DIR"; then
        echo "${RED}${BOLD}Error: Could not change to main project directory '$APP_DIR'.${RESET}"
        exit 1
    fi
    echo "${YELLOW}Current directory for plugin operations: $(pwd)${RESET}"

    # Check if plugin needs to be built
    if [ ! -f "build/build.ninja" ]; then
        echo "${YELLOW}Main plugin build files (build/build.ninja) not found. Performing full build...${RESET}"
        echo "${CYAN}Configuring main project (meson setup)...${RESET}"
        meson setup build --prefix=/usr --buildtype=release --wipe
        echo "${CYAN}Compiling main project (ninja)...${RESET}"
        if ! ninja -C build; then
            echo "${RED}${BOLD}FATAL: Failed to compile main project. Aborting.${RESET}"
            exit 1
        fi
        echo "${GREEN}${BOLD}Main project compiled successfully.${RESET}"
    else
        echo "${YELLOW}Main plugin build files found (build/build.ninja). Skipping build, proceeding to install.${RESET}"
    fi

    # Install the plugin
    echo "${CYAN}Installing main project (ninja install)...${RESET}"
    if ninja -C build install; then
        echo "${GREEN}${BOLD}Main project installed successfully.${RESET}"
    else
        echo "${RED}${BOLD}FATAL: Failed to install main project. Aborting.${RESET}"
        exit 1
    fi

    echo "${YELLOW}Updating system caches after plugin installation...${RESET}"
    ldconfig
    echo "${YELLOW}Clearing GStreamer plugin cache for root user...${RESET}"
    rm -rf /root/.cache/gstreamer-1.0/*

    echo "${CYAN}Verifying 'geminivision' element availability with gst-inspect-1.0...${RESET}"
    if GST_DEBUG="GST_PLUGIN_LOADING:4" gst-inspect-1.0 geminivision > gst_inspect_output.log 2>&1; then
        echo "${GREEN}${BOLD}Plugin element 'geminivision' is available to GStreamer.${RESET}"
        rm gst_inspect_output.log
    else
        echo "${RED}${BOLD}FATAL: Plugin element 'geminivision' NOT FOUND by gst-inspect-1.0 after install.${RESET}"
        echo "${YELLOW}Output of gst-inspect-1.0 geminivision (with GST_PLUGIN_LOADING:4 debug):${RESET}"
        cat gst_inspect_output.log
        exit 1
    fi
    echo "${CYAN}--- Main plugin setup complete ---${RESET}"
}

echo "${CYAN}${BOLD}=== GStreamer Gemini Vision Builder & Tester ===${RESET}"
echo "${YELLOW}Executing action: ${BOLD}$ACTION${RESET}"
if [ "$ACTION" = "test-examples" ] && [ -n "$EXAMPLE_FILE_TO_TEST" ]; then
  echo "${YELLOW}Example to test: ${BOLD}$EXAMPLE_FILE_TO_TEST${RESET}"
fi
echo "----------------------------------------"

case "$ACTION" in
  build)
    echo "${CYAN}Building main project in '$APP_DIR' (will not install)...${RESET}"
    if [ ! -d "$APP_DIR" ]; then
      echo "${RED}${BOLD}Error: Main project directory '$APP_DIR' not found or not mounted.${RESET}"
      exit 1
    fi
    if ! cd "$APP_DIR"; then
      echo "${RED}${BOLD}Error: Could not change to main project directory '$APP_DIR'.${RESET}"
      exit 1
    fi
    echo "${YELLOW}Current directory: $(pwd)${RESET}"
    echo "${CYAN}Configuring main project (meson setup)...${RESET}"
    meson setup build --prefix=/usr --buildtype=release --wipe
    echo "${CYAN}Compiling main project (ninja)...${RESET}"
    if ! ninja -C build; then
        echo "${RED}${BOLD}Failed to build main project.${RESET}"
        exit 1
    fi
    echo "${GREEN}${BOLD}Main project build complete. Artifacts are in '$APP_DIR/build'.${RESET}"
    echo "${YELLOW}Note: Project is built but NOT installed. Use 'build-examples' or 'test-examples' to install.${RESET}"
    ;;

  build-examples)
    ensure_main_plugin_built_and_installed "build-examples"

    echo "${CYAN}Building examples from '$EXAMPLES_MOUNT_DIR'...${RESET}"
    if [ ! -d "$EXAMPLES_MOUNT_DIR" ]; then
      echo "${RED}${BOLD}Error: Examples directory '$EXAMPLES_MOUNT_DIR' not found or not mounted.${RESET}"
      exit 1
    fi
    if ! cd "$EXAMPLES_MOUNT_DIR"; then
      echo "${RED}${BOLD}Error: Could not change to examples directory '$EXAMPLES_MOUNT_DIR'.${RESET}"
      exit 1
    fi
    echo "${YELLOW}Current directory for building examples: $(pwd)${RESET}"
    mkdir -p "$EXAMPLES_OUTPUT_DIR" # $EXAMPLES_MOUNT_DIR/bin

    if [ -f "Makefile" ]; then
      echo "${CYAN}Found Makefile. Attempting to build examples using 'make all'...${RESET}"
      if make all; then # Assumes Makefile outputs to $EXAMPLES_OUTPUT_DIR (e.g., bin/)
        echo "${GREEN}${BOLD}Examples built successfully using Makefile. Artifacts are in '$EXAMPLES_OUTPUT_DIR'.${RESET}"
      else
        echo "${RED}${BOLD}Failed to build examples using Makefile.${RESET}"
        exit 1
      fi
    else
      echo "${YELLOW}Makefile not found. Attempting to compile all .c files into '$EXAMPLES_OUTPUT_DIR'...${RESET}"
      COMPILED_ANY=0
      for c_file in *.c; do
        if [ -f "$c_file" ]; then
          executable_name=$(basename "$c_file" .c)
          echo "${YELLOW}Compiling $c_file -> $EXAMPLES_OUTPUT_DIR/$executable_name ...${RESET}"
          if gcc "$c_file" -o "$EXAMPLES_OUTPUT_DIR/$executable_name" $(pkg-config --cflags --libs gstreamer-1.0); then
            echo "${GREEN}${BOLD}Successfully compiled $c_file${RESET}"
            COMPILED_ANY=1
          else
            echo "${RED}${BOLD}Failed to compile $c_file${RESET}"
            # Optionally, exit 1 here if any single compilation fails
          fi
        fi
      done
      if [ "$COMPILED_ANY" -eq "0" ]; then
         echo "${YELLOW}No .c files found to compile directly, and Makefile missing.${RESET}"
      fi
    fi
    echo "${GREEN}${BOLD}Build examples action complete. Check '$EXAMPLES_OUTPUT_DIR' for artifacts.${RESET}"
    ;;

  test-examples)
    # --- Environment Variable Check for test-examples ---
    if [ -z "$GST_GEMINI_API_KEY" ]; then
      echo "${RED}${BOLD}Error: GST_GEMINI_API_KEY is not set or is empty.${RESET}"; exit 1;
    else
      echo "${GREEN}GST_GEMINI_API_KEY is set.${RESET}"
    fi
    if [ -z "$EXAMPLE_FILE_TO_TEST" ]; then
      echo "${RED}${BOLD}Error: No example file specified for 'test-examples'.${RESET}"; exit 1;
    fi

    ensure_main_plugin_built_and_installed "test-examples"

    echo "${CYAN}Preparing to run example '$EXAMPLE_FILE_TO_TEST' from '$EXAMPLES_MOUNT_DIR'...${RESET}"
    if [ ! -d "$EXAMPLES_MOUNT_DIR" ]; then
      echo "${RED}${BOLD}Error: Examples directory '$EXAMPLES_MOUNT_DIR' not found.${RESET}"; exit 1;
    fi
    if ! cd "$EXAMPLES_MOUNT_DIR"; then
      echo "${RED}${BOLD}Error: Could not change to examples directory '$EXAMPLES_MOUNT_DIR'.${RESET}"; exit 1;
    fi
    echo "${YELLOW}Current directory for testing: $(pwd)${RESET}"
    mkdir -p "$EXAMPLES_OUTPUT_DIR" # Ensure $EXAMPLES_MOUNT_DIR/bin exists

    filename=$(basename -- "$EXAMPLE_FILE_TO_TEST")
    extension="${filename##*.}"
    name_only="${filename%.*}"

    case "$extension" in
      c)
        SOURCE_C_FILE="$filename" # e.g., gemini_vision_example.c
        # Path to the executable within the examples output directory
        TARGET_EXECUTABLE_PATH="$EXAMPLES_OUTPUT_DIR/$name_only" # e.g., bin/gemini_vision_example

        if [ ! -f "$SOURCE_C_FILE" ]; then
            echo "${RED}${BOLD}Error: C source file '$SOURCE_C_FILE' not found in '$EXAMPLES_MOUNT_DIR'.${RESET}"
            exit 1
        fi

        # Check if executable exists, if not, compile it
        if [ ! -f "$TARGET_EXECUTABLE_PATH" ]; then
            echo "${YELLOW}Executable '$TARGET_EXECUTABLE_PATH' not found. Compiling now...${RESET}"
            echo "${CYAN}Compiling C example '$SOURCE_C_FILE' -> '$TARGET_EXECUTABLE_PATH'...${RESET}"
            if gcc "$SOURCE_C_FILE" -o "$TARGET_EXECUTABLE_PATH" $(pkg-config --cflags --libs gstreamer-1.0); then
                echo "${GREEN}${BOLD}Successfully compiled C example: $TARGET_EXECUTABLE_PATH${RESET}"
            else
                echo "${RED}${BOLD}Failed to compile C example '$SOURCE_C_FILE'.${RESET}"
                exit 1
            fi
        else
            echo "${YELLOW}Found existing executable: $TARGET_EXECUTABLE_PATH${RESET}"
        fi

        echo "${CYAN}Running C example: $TARGET_EXECUTABLE_PATH${RESET}"
        "$TARGET_EXECUTABLE_PATH"
        ;;
      py)
        if [ -f "$filename" ]; then
          echo "${CYAN}Running Python example: $filename${RESET}"
          python3 "$filename"
        else
          echo "${RED}${BOLD}Error: Python example script '$filename' not found in '$EXAMPLES_MOUNT_DIR'.${RESET}"
          exit 1
        fi
        ;;
      *)
        echo "${RED}${BOLD}Error: Unknown example file extension '.$extension'.${RESET}"; exit 1;;
    esac
    echo "${GREEN}${BOLD}Example execution finished.${RESET}"
    ;;

  shell)
    echo "${CYAN}Dropping into an interactive shell...${RESET}"
    # ... (shell logic remains the same)
    if [ -n "$GST_GEMINI_API_KEY" ]; then echo "${YELLOW}GST_GEMINI_API_KEY is set.${RESET}"; else echo "${YELLOW}GST_GEMINI_API_KEY is NOT set.${RESET}"; fi
    exec /bin/sh
    ;;
  *)
    echo "${RED}${BOLD}Error: Unknown action '$ACTION'.${RESET}"; exit 1;;
esac
echo "----------------------------------------"