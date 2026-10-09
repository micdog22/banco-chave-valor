cmake_minimum_required(VERSION 3.16)
project(banco_chave_valor VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# A biblioteca: inclua kv.h e ligue com kv.
add_library(kv STATIC src/kv.c)
target_include_directories(kv PUBLIC include)

add_executable(kv_cli src/kv_cli.c)
set_target_properties(kv_cli PROPERTIES OUTPUT_NAME kv)
target_link_libraries(kv_cli PRIVATE kv)

enable_testing()
foreach(nome kv queda)
  add_executable(test_${nome} tests/test_${nome}.c)
  target_link_libraries(test_${nome} PRIVATE kv)
  add_test(NAME ${nome} COMMAND test_${nome})
endforeach()
add_test(NAME cli COMMAND sh ${CMAKE_SOURCE_DIR}/tests/cli.sh ${CMAKE_BINARY_DIR})
