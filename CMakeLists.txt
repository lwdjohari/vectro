cmake_minimum_required(VERSION 3.15)
project(vectro LANGUAGES CXX)


option(VECTRO_USE_CATCH ON)
option(VECTRO_USE_TEST ON)
option(VECTRO_BUNDLE_ABSEIL ON)

set(VECTRO_USE_CATCH ON)
set(VECTRO_USE_TEST ON)
set(VECTRO_BUNDLE_ABSEIL ON)
# -----------------------
# ✅ Project Config
# -----------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "")
    add_subdirectory(third_party/abseil build-abseil)
endif()


# Optional: Silence warnings from Boost headers if needed
# add_compile_options(-isystem ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost)

# -----------------------
# ✅ Source files
# -----------------------
# Manually defined sources
set(VECTRO_SOURCES
  src/vectro/frame/raw_buffer.cc
  src/vectro/frame/internal_message.cc
  src/vectro/plugin/plugin_bundle.cc
#   src/vectro/tls/reloadable_tls_context_provider.cc
#   src/vectro/tls/default_tls_client_context_provider.cc
#   src/vectro/tls/ca_verified_tls_client_context_provider.cc
#   src/vectro/tls/self_signed_tls_client_context_provider.cc
)

# configure_file(
#   ${CMAKE_SOURCE_DIR}/config/vectro-conf.dev.yaml
#   ${CMAKE_BINARY_DIR}/config/vectro-conf.dev.yaml
#   COPYONLY
# )


add_library(${PROJECT_NAME} STATIC ${VECTRO_SOURCES} )
target_link_libraries(${PROJECT_NAME}  
    PUBLIC
        absl::base
        absl::algorithm
        absl::synchronization # absl::Mutex & other concurrency primitives 
    PRIVATE      
    )

# -----------------------
# ✅ Header-only Boost flags
# -----------------------
target_compile_definitions(${PROJECT_NAME} 
    PRIVATE
        BOOST_ASIO_NO_DEPRECATED
        BOOST_ERROR_CODE_HEADER_ONLY
)

# -----------------------
# ✅ Include paths
# -----------------------
target_include_directories(${PROJECT_NAME}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost
        ${CMAKE_CURRENT_SOURCE_DIR}/src/
)

set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

add_library(${PROJECT_NAME}::vectro ALIAS ${PROJECT_NAME} )


if(VECTRO_USE_CATCH AND VECTRO_USE_TEST)
    add_subdirectory(tests build-test)
endif()
