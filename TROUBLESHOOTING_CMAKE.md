# CMake Troubleshooting - Sp3ctra VST

This guide fixes common CMake build issues, in particular the hang on "detecting CXX compile features".

## Problem: CMake hangs on "Detecting CXX compile features"

### Symptoms
- The build stops during the CMake configuration phase
- Message shown: `-- Detecting CXX compile features`
- The process makes no progress for several minutes

### Likely causes

1. **Missing or inaccessible C++ compiler**
2. **Xcode Command Line Tools not installed (macOS)**
3. **Xcode license not accepted (macOS)**
4. **Compiler path conflict**
5. **Corrupted CMake cache**

---

## Solutions (macOS)

### Solution 1: Install the dependencies automatically

Run the provided install script:

```bash
bash scripts/install_dependencies.sh
```

This script will:
- ✅ Check and install the Xcode Command Line Tools
- ✅ Install/update CMake
- ✅ Verify that the compiler is accessible
- ✅ Install the audio dependencies (optional)

---

### Solution 2: Manually install the Xcode Command Line Tools

If the script fails, install them manually:

```bash
# Install the Xcode Command Line Tools
xcode-select --install
```

A dialog window will appear. Follow the installation instructions.

**After installation, verify:**

```bash
# Check the installation
xcode-select -p
# Should print: /Library/Developer/CommandLineTools

# Check the compiler
clang++ --version
# Should print the clang version
```

---

### Solution 3: Accept the Xcode license

If Xcode is installed but the license has not been accepted:

```bash
sudo xcodebuild -license accept
```

---

### Solution 4: Reset the Xcode Command Line Tools

If the compiler is still not detected:

```bash
# Reset the tools
sudo xcode-select --reset

# Then reinstall
xcode-select --install
```

---

### Solution 5: Clear the CMake cache

If CMake has already tried to configure the project:

```bash
# Remove the CMake cache
cd vst
rm -rf build/
mkdir build
cd build

# Reconfigure
cmake ..
```

---

### Solution 6: Force the compiler

If CMake does not detect the compiler automatically:

```bash
cd vst/build

# Specify the compiler explicitly
cmake .. -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++

# Or with the full Xcode path
cmake .. \
  -DCMAKE_C_COMPILER=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++
```

---

## Solutions (Linux - Debian/Ubuntu)

### Solution 1: Install the dependencies automatically

```bash
bash scripts/install_dependencies.sh
```

---

### Solution 2: Manual install

```bash
# Update the packages
sudo apt update

# Install build-essential (includes gcc/g++)
sudo apt install -y build-essential

# Install CMake
sudo apt install -y cmake

# Check the compiler
g++ --version

# Check CMake
cmake --version
```

---

### Solution 3: Install a more recent version of CMake

If your CMake version is too old (< 3.15):

```bash
# Uninstall the old version
sudo apt remove cmake

# Add the Kitware repository (official CMake)
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | \
  gpg --dearmor - | \
  sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null

# Add the repository to sources.list
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ focal main' | \
  sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null

# Install a recent CMake
sudo apt update
sudo apt install -y cmake

# Check the version
cmake --version
```

---

## Full diagnostics

### Check the build environment

Run these commands to diagnose your environment:

```bash
# 1. Check the operating system
uname -a

# 2. Check CMake
cmake --version

# 3. Check the C++ compiler
which clang++    # macOS
which g++        # Linux
clang++ --version   # macOS
g++ --version       # Linux

# 4. Check Xcode (macOS only)
xcode-select -p

# 5. Test a simple compilation
echo 'int main() { return 0; }' > test.cpp
clang++ test.cpp -o test   # macOS
g++ test.cpp -o test       # Linux
./test && echo "Compilation OK" || echo "Compilation FAILED"
rm test.cpp test

# 6. Check the environment variables
echo $PATH
echo $CXX
echo $CC
```

---

## Full troubleshooting procedure

### Step 1: Clean everything

```bash
# Remove all build files
cd /Users/zhonx/Documents/Workspaces/Workspace_Ondulab/Sp3ctra_VST
rm -rf vst/build/
rm -rf build/
```

### Step 2: Install the dependencies

```bash
# Run the install script
bash scripts/install_dependencies.sh
```

### Step 3: Verify the installation

The script will print a verification report. Make sure that:
- ✅ CMake version >= 3.15
- ✅ C++ compiler detected (clang++ or g++)
- ✅ Git installed

### Step 4: Build the project

```bash
# Option A: Use the build script
bash scripts/build_vst.sh

# Option B: Manual build
cd vst
mkdir -p build && cd build
cmake ..
cmake --build . --config Release
```

---

## Common error messages

### "No CMAKE_CXX_COMPILER could be found"

**Solution:** The compiler is not installed or not in the PATH.

```bash
# macOS
xcode-select --install

# Linux
sudo apt install build-essential
```

---

### "CMake Error: your CXX compiler is not able to compile a simple test program"

**Possible causes:**
1. Xcode license not accepted (macOS)
2. Missing system libraries
3. Corrupted compiler

**Solutions:**

```bash
# macOS: Accept the license
sudo xcodebuild -license accept

# macOS: Reinstall the Command Line Tools
sudo rm -rf /Library/Developer/CommandLineTools
xcode-select --install

# Linux: Reinstall build-essential
sudo apt install --reinstall build-essential
```

---

### Timeout on "Detecting CXX compile features"

**Causes:**
- Network problem (if CMake is trying to download dependencies)
- Non-functional compiler
- Stuck process

**Solutions:**

```bash
# 1. Kill the stuck CMake processes
killall cmake

# 2. Clear the cache
rm -rf vst/build/

# 3. Verify that the compiler works
clang++ --version  # Must print a version with no error

# 4. Re-run in verbose mode
cd vst/build
cmake .. --debug-output
```

---

## Successful build

When CMake configures correctly, you will see:

```
-- The CXX compiler identification is AppleClang X.X.X  (or GNU X.X.X on Linux)
-- Detecting CXX compiler ABI info
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: /usr/bin/clang++ - works
-- Detecting CXX compile features
-- Detecting CXX compile features - done
```

Then JUCE will be downloaded automatically:

```
-- Fetching JUCE...
-- JUCE framework downloaded successfully
```

---

## Additional support

If the problem persists after following this guide:

1. **Collect the diagnostic information**:
   ```bash
   cmake --version > diagnostic.txt
   clang++ --version >> diagnostic.txt  # or g++ --version
   xcode-select -p >> diagnostic.txt    # macOS only
   uname -a >> diagnostic.txt
   ```

2. **Check the CMake logs**:
   ```bash
   cd vst/build
   cat CMakeFiles/CMakeError.log
   cat CMakeFiles/CMakeOutput.log
   ```

3. **Share this information** with your development team

---

## Quick command summary

### Quick diagnostics (macOS)

```bash
# Full check in one command
bash scripts/install_dependencies.sh
```

### Quick diagnostics (Linux)

```bash
# Full check in one command
bash scripts/install_dependencies.sh
```

### Clean and rebuild

```bash
# Clean
rm -rf vst/build/

# Rebuild
bash scripts/build_vst.sh
```

---

**Note:** The JUCE framework is downloaded automatically by CMake via `FetchContent`. No manual JUCE installation is required.
