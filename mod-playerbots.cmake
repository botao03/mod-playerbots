# Included by the core's modules/CMakeLists.txt after the `modules` target exists.
#
# The core links the MySQL client library privately into its `database` target,
# so its include directories do not reach module code. PlayerbotsDatabase.cpp
# needs the complete MySQLPreparedStatement type (which pulls in mysql.h) and
# Playerbots.cpp uses ER_BAD_DB_ERROR from mysqld_error.h. Linking the imported
# `mysql` target here propagates those include directories to the module build.
target_link_libraries(modules
  PRIVATE
    mysql)

# Cache the shared AI headers during large local rebuilds; optional for other toolchains.
option(PLAYERBOTS_USE_PCH "Precompile playerbot headers for the static modules target" OFF)
if (PLAYERBOTS_USE_PCH AND MODULES STREQUAL "static")
  target_precompile_headers(modules PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src/Script/Playerbots.h")
endif()
