cmake_minimum_required(VERSION 3.28)
project(myapp LANGUAGES CXX)

# This is the naive, honest consumer wiring: exactly what a developer would write after reading the
# AgentEngine API docs, which document no consumption story at all. Vendored engine, one link line.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." agentengine)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE agentengine::core)
