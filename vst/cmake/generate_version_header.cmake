# Generates Sp3ctraVersion.h from vst/VERSION (full x.y.z).
#
# Invoked two ways from CMakeLists.txt:
#   - at configure time with INCREMENT=OFF (header exists for IDE indexing)
#   - at build time  with INCREMENT=ON  (bumps the patch digit z on every build,
#     written back into vst/VERSION)
#
# Required -D arguments:
#   VERSION_FILE   path to the version file (e.g. "1.1.0")
#   OUTPUT_HEADER  path of the header to generate
#   INCREMENT      ON to bump the patch number, OFF to just regenerate

file(READ "${VERSION_FILE}" SP3CTRA_VERSION)
string(STRIP "${SP3CTRA_VERSION}" SP3CTRA_VERSION)

if(NOT SP3CTRA_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "vst/VERSION must contain x.y.z, got '${SP3CTRA_VERSION}'")
endif()
set(SP3CTRA_VERSION_MAJOR ${CMAKE_MATCH_1})
set(SP3CTRA_VERSION_MINOR ${CMAKE_MATCH_2})
set(SP3CTRA_VERSION_PATCH ${CMAKE_MATCH_3})

if(INCREMENT)
    math(EXPR SP3CTRA_VERSION_PATCH "${SP3CTRA_VERSION_PATCH} + 1")
    set(SP3CTRA_VERSION "${SP3CTRA_VERSION_MAJOR}.${SP3CTRA_VERSION_MINOR}.${SP3CTRA_VERSION_PATCH}")
    file(WRITE "${VERSION_FILE}" "${SP3CTRA_VERSION}\n")
endif()

string(TIMESTAMP SP3CTRA_BUILD_DATE "%Y-%m-%d %H:%M")

file(WRITE "${OUTPUT_HEADER}"
"// Auto-generated at build time by cmake/generate_version_header.cmake — do not edit.
// Version comes from vst/VERSION; the patch digit auto-increments on every build.
#pragma once

#define SP3CTRA_VERSION_MAJOR  ${SP3CTRA_VERSION_MAJOR}
#define SP3CTRA_VERSION_MINOR  ${SP3CTRA_VERSION_MINOR}
#define SP3CTRA_VERSION_PATCH  ${SP3CTRA_VERSION_PATCH}
#define SP3CTRA_BUILD_DATE     \"${SP3CTRA_BUILD_DATE}\"
#define SP3CTRA_VERSION_STRING \"${SP3CTRA_VERSION}\"
")

if(INCREMENT)
    message(STATUS "Sp3ctra version: ${SP3CTRA_VERSION}")
endif()
