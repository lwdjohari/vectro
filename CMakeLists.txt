cmake_minimum_required(VERSION 3.15)
project(vectro LANGUAGES CXX)

set(VECTRO_PROJECT_VERSION_MAJOR 0)
set(VECTRO_PROJECT_VERSION_MINOR 3)
set(VECTRO_PROJECT_VERSION_PATCH 0)
set(VECTRO_PROJECT_VERSION
    "${VECTRO_PROJECT_VERSION_MAJOR}.${VECTRO_PROJECT_VERSION_MINOR}.${VECTRO_PROJECT_VERSION_PATCH}"
)

message(STATUS "Vectro v${VECTRO_PROJECT_VERSION}")
message(STATUS "--------------------------------")

option(VECTRO_BUILD_SHARED ON)
option(VECTRO_USE_CATCH ON)
option(VECTRO_USE_TEST ON)
option(VECTRO_BUNDLE_ABSEIL ON)

set(VECTRO_USE_CATCH ON)
set(VECTRO_USE_TEST ON)
set(VECTRO_BUNDLE_ABSEIL ON)


# Mobile detection helpers
set(VECTRO_PLATFORM_IOS FALSE)
set(VECTRO_PLATFORM_ANDROID FALSE)
set(VECTRO_PLATFORM_DESKTOP FALSE)


if(ANDROID)
    set(VECTRO_PLATFORM_ANDROID TRUE)
elseif(APPLE)
    # CMake's iOS detection is via CMAKE_SYSTEM_NAME or SDK
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_OSX_SYSROOT MATCHES "iphoneos")
        set(VECTRO_PLATFORM_IOS TRUE)
    else()
        set(VECTRO_PLATFORM_DESKTOP TRUE) # macOS
    endif()
else()
    set(VECTRO_PLATFORM_DESKTOP TRUE)     # Windows / Linux / BSD / etc.
endif()

# Mobile platforms: force static libs
if(VECTRO_PLATFORM_IOS OR VECTRO_PLATFORM_ANDROID)
    set(VECTRO_BUILD_SHARED OFF)
endif()

# 2. Output layout: bin/ + lib/
# ---------------------------------------------------------------------------
# We want:
#   build/bin  -> all executables + shared libs (if any)
#   build/lib  -> all static libs

set(VECTRO_BIN_DIR  "${CMAKE_BINARY_DIR}/bin")
set(VECTRO_LIB_DIR  "${CMAKE_BINARY_DIR}/lib")

# Single-config generators (Ninja, Makefiles)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY  "${VECTRO_BIN_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY  "${VECTRO_BIN_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY  "${VECTRO_LIB_DIR}")

# Multi-config generators (Visual Studio, Xcode) – keep consistent layout
foreach(cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" cfg_uc)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_uc}  "${VECTRO_BIN_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg_uc}  "${VECTRO_BIN_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfg_uc}  "${VECTRO_LIB_DIR}")
endforeach()

# 3. RPATH / loader behavior (desktop only)
# ---------------------------------------------------------------------------

if(VECTRO_PLATFORM_DESKTOP)

    # We want the exe to look for .so/.dylib in its own directory:
    #   Linux  -> $ORIGIN
    #   macOS  -> @loader_path  (CMake maps $ORIGIN automatically)

    set(BUILD_SHARED_LIBS ON CACHE BOOL "Build all libs as shared" FORCE)

    if(UNIX OR APPLE)
        set(CMAKE_SKIP_BUILD_RPATH  FALSE)
        set(CMAKE_SKIP_INSTALL_RPATH FALSE)
        set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)

        # Make RPATH relative to the executable location
        set(CMAKE_BUILD_RPATH  "$ORIGIN")
        set(CMAKE_INSTALL_RPATH "$ORIGIN")

        # We *don't* want system search paths baked into RPATH;
        # Only the relative one above.
        set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
    endif()

    # Windows doesn’t need RPATH: it always searches EXE dir first for DLLs.
endif()

# Mobile: no RPATH, everything is static (iOS) or NDK-packaged (Android).
if(VECTRO_PLATFORM_IOS OR VECTRO_PLATFORM_ANDROID)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build all libs as static" FORCE)
    set(CMAKE_SKIP_BUILD_RPATH  TRUE)
    set(CMAKE_SKIP_INSTALL_RPATH TRUE)
endif()

# 4. Compiler basics / C++ standard (tune as you like)
# ---------------------------------------------------------------------------

# -----------------------
# ✅ Project Config
# -----------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()



# -----------------------
# ✅ Catch2 Lib
# -----------------------
if(VECTRO_USE_CATCH)
 message(STATUS  "CATCH2::ADD_LIB.")
 find_package(Catch2 3 REQUIRED)
endif()

# -----------------------
# ✅ Threads Lib
# -----------------------
find_package(Threads REQUIRED)

# -----------------------
# ✅ Google Abseil
# -----------------------

if(VECTRO_BUNDLE_ABSEIL)
    message(STATUS  "Vectro:    Abseil - Use from bundle.")
    # Optional: stricter control for Abseil itself; these options exist in Abseil CMake.
    set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/abseil build-abseil)
endif()

# -----------------------
# ✅ OpenSSL Lib
# -----------------------

find_package(OpenSSL REQUIRED)

if (NOT OpenSSL_FOUND)
  message(FATAL_ERROR "OpenSSL not found, but it is required")
endif()

# CMake's FindOpenSSL usually gives OPENSSL_VERSION like "3.0.14"
if (NOT DEFINED OPENSSL_VERSION)
  message(FATAL_ERROR "OPENSSL_VERSION is not defined; cannot verify OpenSSL major version. \
This environment is not supported (need OpenSSL 3.x).")
endif()

if (OPENSSL_VERSION MATCHES "LibreSSL")
  message(FATAL_ERROR "LibreSSL is not supported; require OpenSSL 3.x.")
endif()

if (OPENSSL_VERSION MATCHES "BoringSSL")
  message(FATAL_ERROR "BoringSSL is not supported; require OpenSSL 3.x.")
endif()

# Extract major version safely (before first dot)
string(REGEX MATCH "^[0-9]+" OPENSSL_VERSION_MAJOR "${OPENSSL_VERSION}")

if (NOT OPENSSL_VERSION_MAJOR STREQUAL "3")
  message(FATAL_ERROR
    "OpenSSL 3.x required, but found ${OPENSSL_VERSION} (major=${OPENSSL_VERSION_MAJOR}).")
endif()

message(STATUS "Using OpenSSL ${OPENSSL_VERSION} (major ${OPENSSL_VERSION_MAJOR})")

# -----------------------
# ✅ Asio Lib
# -----------------------


# third_party/asio/CMakeLists.txt (or top-level if you prefer)
set(ASIO_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/asio/include")
add_library(vasio INTERFACE)
target_include_directories(vasio
    INTERFACE
        ${ASIO_INCLUDE_DIR}   # path that contains asio.hpp
        
)

target_compile_definitions(vasio
    INTERFACE
        ASIO_STANDALONE        # use standalone, not boost::asio
        ASIO_HEADER_ONLY       # header-only mode
        # ASIO_NO_EXCEPTIONS
        ASIO_HAS_STD_CHRONO
        ASIO_HAS_STD_SHARED_PTR
        ASIO_HAS_STD_FUNCTION
        ASIO_NO_DEPRECATED     # optional, to keep APIs clean
        ASIO_HAS_STD_ADDRESSOF
        ASIO_HAS_STD_ARRAY
        ASIO_HAS_CSTDINT
        ASIO_HAS_OPENSSL
)

target_link_libraries(vasio
    INTERFACE
        OpenSSL::SSL
        OpenSSL::Crypto
)

# POSIX: you probably want pthread for anything serious
# if(UNIX AND NOT APPLE)
#     target_link_libraries(vasio INTERFACE pthread)
# endif()
# -----------------------
# ✅ Source files
# -----------------------
# Manually defined sources
set(VECTRO_SOURCES
  src/vectro/frame/raw_buffer.cc
  src/vectro/frame/internal_message.cc
  src/vectro/plugin/plugin_bundle.cc
  src/vectro/tls/reloadable_tls_context_provider.cc
  src/vectro/tls/default_tls_client_context_provider.cc
  src/vectro/tls/default_tls_client_context_provider.cc
  src/vectro/tls/ca_verified_tls_client_context_provider.cc
  src/vectro/tls/self_signed_tls_client_context_provider.cc
)

# configure_file(
#   ${CMAKE_SOURCE_DIR}/config/vectro-conf.dev.yaml
#   ${CMAKE_BINARY_DIR}/config/vectro-conf.dev.yaml
#   COPYONLY
# )


add_library(${PROJECT_NAME}  ${VECTRO_SOURCES} )
target_link_libraries(${PROJECT_NAME}  
    PUBLIC
        pthread
        vasio
        absl::base
        absl::algorithm
        absl::time
        absl::strings
        absl::synchronization 
        absl::base
        absl::algorithm
        absl::time
        absl::strings
        absl::synchronization 
        absl::flat_hash_map        
        absl::node_hash_map     
        absl::btree             
        absl::function_ref      
    INTERFACE
        OpenSSL::SSL
        OpenSSL::Crypto
    )

# --- VERSIONED SHARED LIB WITH SOFTLINKS ---
set_target_properties(vectro PROPERTIES
    VERSION   ${VECTRO_PROJECT_VERSION}          
    SOVERSION ${VECTRO_PROJECT_VERSION_MAJOR}    
    PUBLIC_HEADER "${CMAKE_SOURCE_DIR}/src/vectro/*.h"
)

# target_compile_definitions(${PROJECT_NAME}
#     INTERFACE
#         ASIO_STANDALONE        # use standalone, not boost::asio
#         ASIO_HEADER_ONLY       # header-only mode
#         ASIO_HAS_STD_CHRONO
#         ASIO_HAS_STD_SHARED_PTR
#         ASIO_HAS_STD_FUNCTION
#         ASIO_NO_DEPRECATED     # optional, to keep APIs clean
# )

# -----------------------
# ✅ Include paths
# -----------------------
target_include_directories(${PROJECT_NAME}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/
    
)

set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

add_library(${PROJECT_NAME}::vectro ALIAS ${PROJECT_NAME} )


if(VECTRO_USE_CATCH AND VECTRO_USE_TEST)
    add_subdirectory(tests build-test)
endif()
