# Generates Sp3ctraVersion.h from vst/VERSION + vst/BUILD_NUMBER.
#
# Invoked two ways from CMakeLists.txt:
#   - at configure time with INCREMENT=OFF (header exists for IDE indexing)
#   - at build time  with INCREMENT=ON  (bumps BUILD_NUMBER on every build)
#
# Required -D arguments:
#   VERSION_FILE       path to the base-version file (e.g. "0.1.5")
#   BUILD_NUMBER_FILE  path to the build counter file (auto-incremented)
#   OUTPUT_HEADER      path of the header to generate
#   INCREMENT          ON to bump the counter, OFF to just regenerate

file(READ "${VERSION_FILE}" SP3CTRA_BASE_VERSION)
string(STRIP "${SP3CTRA_BASE_VERSION}" SP3CTRA_BASE_VERSION)

set(SP3CTRA_BUILD_NUMBER 0)
if(EXISTS "${BUILD_NUMBER_FILE}")
    file(READ "${BUILD_NUMBER_FILE}" SP3CTRA_BUILD_NUMBER)
    string(STRIP "${SP3CTRA_BUILD_NUMBER}" SP3CTRA_BUILD_NUMBER)
    if(NOT SP3CTRA_BUILD_NUMBER MATCHES "^[0-9]+$")
        set(SP3CTRA_BUILD_NUMBER 0)
    endif()
endif()

if(INCREMENT)
    math(EXPR SP3CTRA_BUILD_NUMBER "${SP3CTRA_BUILD_NUMBER} + 1")
    file(WRITE "${BUILD_NUMBER_FILE}" "${SP3CTRA_BUILD_NUMBER}\n")
endif()

string(TIMESTAMP SP3CTRA_BUILD_DATE "%Y-%m-%d %H:%M")

file(WRITE "${OUTPUT_HEADER}"
"// Auto-generated at build time by cmake/generate_version_header.cmake — do not edit.
// Base version comes from vst/VERSION; build number from vst/BUILD_NUMBER.
#pragma once

#define SP3CTRA_VERSION_BASE   \"${SP3CTRA_BASE_VERSION}\"
#define SP3CTRA_BUILD_NUMBER   ${SP3CTRA_BUILD_NUMBER}
#define SP3CTRA_BUILD_DATE     \"${SP3CTRA_BUILD_DATE}\"
#define SP3CTRA_VERSION_STRING \"${SP3CTRA_BASE_VERSION} (b${SP3CTRA_BUILD_NUMBER})\"
")

if(INCREMENT)
    message(STATUS "Sp3ctra version: ${SP3CTRA_BASE_VERSION} build ${SP3CTRA_BUILD_NUMBER}")
endif()
