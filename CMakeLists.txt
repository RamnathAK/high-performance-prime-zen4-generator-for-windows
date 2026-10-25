cmake_minimum_required(VERSION 3.20)
project(prime_zen4 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Release by default
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(prime src/hp_prime_stream_win.c)

# Windows libs
if(WIN32)
  target_link_libraries(prime PRIVATE synchronization)
endif()

# MSVC flags
if(MSVC)
  target_compile_options(prime PRIVATE /O2 /arch:AVX2 /W3 /GL)
  target_link_options(prime PRIVATE /LTCG)
else()
  # clang-cl or clang
  target_compile_options(prime PRIVATE -O3 -march=znver4 -Wall -Wextra)
endif()

# Output to build/
set_target_properties(prime PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Install rule
install(TARGETS prime DESTINATION bin)
