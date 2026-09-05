cmake_minimum_required(VERSION 3.24)

project(pg_unitree_r1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "pg_unitree_r1 supports Linux only")
endif()

set(EXTENSION_CONTROL "${CMAKE_CURRENT_SOURCE_DIR}/pg_unitree_r1.control")
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${EXTENSION_CONTROL}"
)

file(READ "${EXTENSION_CONTROL}" extension_control)
string(
  REGEX MATCH
  "default_version[ \t]*=[ \t]*'([^']+)'"
  extension_version_match
  "${extension_control}"
)
set(PG_UNITREE_R1_VERSION "${CMAKE_MATCH_1}")
if(PG_UNITREE_R1_VERSION STREQUAL "")
  message(FATAL_ERROR "Could not read default_version from pg_unitree_r1.control")
endif()

find_program(PG_CONFIG_EXECUTABLE NAMES pg_config REQUIRED)

function(pg_config_query output_variable option)
  execute_process(
    COMMAND "${PG_CONFIG_EXECUTABLE}" "${option}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "pg_config ${option} failed: ${error}")
  endif()
  set("${output_variable}" "${output}" PARENT_SCOPE)
endfunction()

pg_config_query(POSTGRES_VERSION_STRING --version)
string(REGEX MATCH "^PostgreSQL ([0-9]+)" postgres_version_match "${POSTGRES_VERSION_STRING}")
set(POSTGRES_MAJOR "${CMAKE_MATCH_1}")
if(NOT POSTGRES_MAJOR STREQUAL "18")
  message(FATAL_ERROR "pg_unitree_r1 requires PostgreSQL 18; found ${POSTGRES_VERSION_STRING}")
endif()

pg_config_query(POSTGRES_INCLUDEDIR --includedir)
pg_config_query(POSTGRES_INCLUDEDIR_SERVER --includedir-server)
pg_config_query(POSTGRES_PKGLIBDIR --pkglibdir)
pg_config_query(POSTGRES_SHAREDIR --sharedir)

include(FetchContent)

set(
  PG_UNITREE_R1_SDK2_REPOSITORY
  "https://github.com/unitreerobotics/unitree_sdk2.git"
  CACHE STRING
  "Unitree SDK2 Git repository"
)
set(
  PG_UNITREE_R1_SDK2_GIT_TAG
  "9754cd153af3da471b0fe5f3aa535e426fb11db3"
  CACHE STRING
  "Pinned Unitree SDK2 Git revision"
)

FetchContent_Declare(
  unitree_sdk2
  GIT_REPOSITORY "${PG_UNITREE_R1_SDK2_REPOSITORY}"
  GIT_TAG "${PG_UNITREE_R1_SDK2_GIT_TAG}"
  GIT_SHALLOW FALSE
  GIT_PROGRESS TRUE
  SOURCE_SUBDIR _pg_unitree_r1_fetch_only
)
FetchContent_MakeAvailable(unitree_sdk2)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" system_processor)
if(system_processor MATCHES "^(amd64|x86_64)$")
  set(UNITREE_ARCH x86_64)
elseif(system_processor MATCHES "^(aarch64|arm64)$")
  set(UNITREE_ARCH aarch64)
else()
  message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(UNITREE_SDK_LIBRARY "${unitree_sdk2_SOURCE_DIR}/lib/${UNITREE_ARCH}/libunitree_sdk2.a")
set(UNITREE_DDS_LIBRARY_DIR "${unitree_sdk2_SOURCE_DIR}/thirdparty/lib/${UNITREE_ARCH}")
set(UNITREE_DDS_C_LIBRARY "${UNITREE_DDS_LIBRARY_DIR}/libddsc.so")
set(UNITREE_DDS_CXX_LIBRARY "${UNITREE_DDS_LIBRARY_DIR}/libddscxx.so")

foreach(
  dependency_file
  IN ITEMS
    "${UNITREE_SDK_LIBRARY}"
    "${UNITREE_DDS_C_LIBRARY}"
    "${UNITREE_DDS_CXX_LIBRARY}"
)
  if(NOT EXISTS "${dependency_file}")
    message(FATAL_ERROR "Unitree SDK2 dependency is missing: ${dependency_file}")
  endif()
endforeach()

find_package(Threads REQUIRED)

add_library(UnitreeSDK2::ddsc SHARED IMPORTED)
set_target_properties(
  UnitreeSDK2::ddsc
  PROPERTIES IMPORTED_LOCATION "${UNITREE_DDS_C_LIBRARY}"
)

add_library(UnitreeSDK2::ddscxx SHARED IMPORTED)
set_target_properties(
  UnitreeSDK2::ddscxx
  PROPERTIES IMPORTED_LOCATION "${UNITREE_DDS_CXX_LIBRARY}"
)

add_library(UnitreeSDK2::unitree_sdk2 STATIC IMPORTED)
set_target_properties(
  UnitreeSDK2::unitree_sdk2
  PROPERTIES
    IMPORTED_LOCATION "${UNITREE_SDK_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES
      "${unitree_sdk2_SOURCE_DIR}/include;${unitree_sdk2_SOURCE_DIR}/thirdparty/include;${unitree_sdk2_SOURCE_DIR}/thirdparty/include/ddscxx"
)

add_library(
  pg_unitree_r1
  MODULE
  src/extension.cpp
  src/unitree_gateway.cpp
  src/control_core.cpp
)
set_target_properties(
  pg_unitree_r1
  PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    INSTALL_RPATH "$ORIGIN"
)
target_compile_definitions(
  pg_unitree_r1
  PRIVATE
    _GNU_SOURCE
    PG_UNITREE_R1_VERSION=\"${PG_UNITREE_R1_VERSION}\"
)
target_compile_options(pg_unitree_r1 PRIVATE -Wall -Wextra -Wformat-security -fno-strict-aliasing -fwrapv)
target_include_directories(
  pg_unitree_r1
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${POSTGRES_INCLUDEDIR_SERVER}"
    "${POSTGRES_INCLUDEDIR}/internal"
)
target_link_libraries(
  pg_unitree_r1
  PRIVATE
    UnitreeSDK2::unitree_sdk2
    UnitreeSDK2::ddscxx
    UnitreeSDK2::ddsc
    Threads::Threads
    "${CMAKE_DL_LIBS}"
    rt
)

include(CTest)
if(BUILD_TESTING)
  add_executable(
    control_core_test
    tests/control_core_test.cpp
    src/control_core.cpp
  )
  target_include_directories(control_core_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_options(control_core_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME control_core_test COMMAND control_core_test)
endif()

set(EXTENSION_SQL "${CMAKE_CURRENT_SOURCE_DIR}/sql/pg_unitree_r1--${PG_UNITREE_R1_VERSION}.sql")
if(NOT EXISTS "${EXTENSION_SQL}")
  message(FATAL_ERROR "Missing extension install script: ${EXTENSION_SQL}")
endif()

install(TARGETS pg_unitree_r1 LIBRARY DESTINATION "${POSTGRES_PKGLIBDIR}")
install(
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/pg_unitree_r1.control"
    "${EXTENSION_SQL}"
  DESTINATION "${POSTGRES_SHAREDIR}/extension"
)
install(FILES "${UNITREE_DDS_C_LIBRARY}" DESTINATION "${POSTGRES_PKGLIBDIR}" RENAME libddsc.so.0)
install(FILES "${UNITREE_DDS_C_LIBRARY}" DESTINATION "${POSTGRES_PKGLIBDIR}" RENAME libddsc.so)
install(FILES "${UNITREE_DDS_CXX_LIBRARY}" DESTINATION "${POSTGRES_PKGLIBDIR}" RENAME libddscxx.so.0)
install(FILES "${UNITREE_DDS_CXX_LIBRARY}" DESTINATION "${POSTGRES_PKGLIBDIR}" RENAME libddscxx.so)
