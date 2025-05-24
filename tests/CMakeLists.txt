cmake_minimum_required(VERSION 3.15)
project(vectro-unit-test CXX)

# Set the path to the directory containing your test source files
set(TEST_SOURCES
    frame/rawbuffer/rawbuffer_test.cc
    # Add more test files if needed
)

# Create an executable for the tests
add_executable(${PROJECT_NAME} ${TEST_SOURCES})

# Add Include location
target_include_directories(${PROJECT_NAME}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/
)

# Link the Catch2 library to the test executable
target_link_libraries(${PROJECT_NAME} PUBLIC Catch2::Catch2WithMain vectro::vectro)

# Set the C++ standard
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)