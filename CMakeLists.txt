cmake_minimum_required(VERSION 3.15)
project(vectro LANGUAGES CXX)

# -----------------------
# ✅ Project Config
# -----------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------
# ✅ Header-only Boost flags
# -----------------------
add_compile_definitions(
    BOOST_ASIO_NO_DEPRECATED
    BOOST_ERROR_CODE_HEADER_ONLY
)

# Optional: Silence warnings from Boost headers if needed
# add_compile_options(-isystem ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost)

# -----------------------
# ✅ Source files
# -----------------------
# file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cc)
file(GLOB_RECURSE SOURCES_VECTRO CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.s
)

add_library(${PROJECT_NAME} STATIC ${SOURCES_VECTRO} )
# target_link_libraries(${PROJECT_NAME}  
#     PUBLIC
#         ${PROJECT_NAME}::core 
#     PRIVATE      
#     )

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

