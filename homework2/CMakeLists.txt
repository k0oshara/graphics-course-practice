cmake_minimum_required(VERSION 3.0)
project(homework2)

cmake_policy(SET CMP0072 NEW)
cmake_policy(SET CMP0074 NEW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/modules")

find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(SDL2 REQUIRED)

add_subdirectory(glm)

set(TARGET_NAME "${PROJECT_NAME}")

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(${TARGET_NAME}
    main.cpp
)

target_include_directories(${TARGET_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${SDL2_INCLUDE_DIRS}"
    "${GLEW_INCLUDE_DIRS}"
    "${OPENGL_INCLUDE_DIRS}"
)

target_link_libraries(${TARGET_NAME} PUBLIC
    glm
    "${SDL2_LIBRARIES}"
    "${GLEW_LIBRARIES}"
    "${OPENGL_LIBRARIES}"
)

target_compile_definitions(${TARGET_NAME} PUBLIC -DPROJECT_ROOT="${PROJECT_ROOT}")
