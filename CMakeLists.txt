cmake_minimum_required(VERSION 3.28)

project(
  ssd-cache
  VERSION 0.1.0
  DESCRIPTION "Windows native NAS cache disk service"
  LANGUAGES C CXX
)

option(SSD_CACHE_BUILD_TESTS "Build test executables" ON)
option(SSD_CACHE_DEVELOPMENT_USER_MODE_ONLY "Skip the required WDK driver" OFF)
option(SSD_CACHE_BUILD_INSTALLER "Enable CPack installer generation" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(
  sqlite3
  PRIVATE
    SQLITE_ENABLE_COLUMN_METADATA
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_RTREE
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
)

add_library(
  ssd_cache_core
  src/core/access_event.cpp
  src/core/cache_eviction.cpp
  src/core/cache_index.cpp
  src/core/config.cpp
  src/core/job_scheduler.cpp
  src/core/mode_manager.cpp
  src/core/path_mapper.cpp
  src/core/time.cpp
  src/core/utf.cpp
)
target_include_directories(ssd_cache_core PUBLIC src/core/include)
target_link_libraries(ssd_cache_core PUBLIC sqlite3)

if(WIN32)
  set(
    SSD_CACHE_TRAY_ICON_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/tray_icons/driver_running.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/tray_icons/driver_warning.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/tray_icons/driver_missing.png"
  )

  add_library(
    ssd_cache_win
    src/platform/win/win_config_paths.cpp
    src/platform/win/win_file_copy.cpp
    src/platform/win/win_file_logger.cpp
    src/platform/win/win_filter_activity_source.cpp
    src/platform/win/win_mount_manager.cpp
    src/platform/win/win_service_controller.cpp
    src/platform/win/win_service_host.cpp
  )
  target_include_directories(ssd_cache_win PUBLIC src/platform/win/include src/shared)
  target_compile_definitions(ssd_cache_win PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(
    ssd_cache_win
    PUBLIC
      ssd_cache_core
      Advapi32
      Bcrypt
      FltLib
      Mpr
      Shell32
  )

  add_executable(ssd-cache-service WIN32 src/service/service_main.cpp)
  target_link_libraries(ssd-cache-service PRIVATE ssd_cache_core ssd_cache_win)

  add_executable(ssd-cache-tray WIN32 src/app/tray_main.cpp)
  target_link_libraries(
    ssd-cache-tray
    PRIVATE
      ssd_cache_core
      ssd_cache_win
      Gdiplus
  )
  foreach(icon_file IN LISTS SSD_CACHE_TRAY_ICON_FILES)
    add_custom_command(
      TARGET ssd-cache-tray
      POST_BUILD
      COMMAND
        "${CMAKE_COMMAND}"
        -E
        copy_if_different
        "${icon_file}"
        "$<TARGET_FILE_DIR:ssd-cache-tray>"
      VERBATIM
    )
  endforeach()
endif()

if(SSD_CACHE_BUILD_TESTS)
  enable_testing()

  add_executable(
    ssd-cache-core-tests
    tests/core/cache_eviction_test.cpp
    tests/core/cache_index_test.cpp
    tests/core/config_test.cpp
    tests/core/job_scheduler_test.cpp
    tests/core/path_mapper_test.cpp
    tests/test_main.cpp
  )
  target_link_libraries(ssd-cache-core-tests PRIVATE ssd_cache_core)
  target_include_directories(ssd-cache-core-tests PRIVATE tests)
  add_test(NAME ssd-cache-core-tests COMMAND ssd-cache-core-tests)
endif()

if(NOT SSD_CACHE_DEVELOPMENT_USER_MODE_ONLY)
  if(NOT WIN32)
    message(FATAL_ERROR "The minifilter driver can only be built on Windows.")
  endif()

  find_program(MSBUILD_EXE MSBuild.exe)
  if(NOT MSBUILD_EXE)
    message(
      FATAL_ERROR
      "MSBuild.exe is required for the required WDK driver build."
    )
  endif()

  find_program(POWERSHELL_EXE powershell.exe REQUIRED)

  add_custom_target(
    cachemon-driver ALL
    COMMAND
      "${MSBUILD_EXE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/driver/cachemon_minifilter.vcxproj"
      /p:Configuration=$<CONFIG>
      /p:Platform=x64
    COMMAND
      "${POWERSHELL_EXE}"
      -NoProfile
      -ExecutionPolicy
      Bypass
      -File
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-driver-direct.ps1"
      -Configuration
      $<CONFIG>
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM
  )
else()
  message(
    WARNING
    "Building without the WDK minifilter. This is only for user-mode "
    "development; it cannot populate the access database in production."
  )
endif()

if(TARGET ssd_cache_win)
  install(TARGETS ssd-cache-service ssd-cache-tray RUNTIME DESTINATION bin)
  install(FILES ${SSD_CACHE_TRAY_ICON_FILES} DESTINATION bin)
  install(FILES README.md DESTINATION .)
  install(FILES LICENSE DESTINATION .)
  install(FILES config/config.ini.sample DESTINATION config)
  install(FILES scripts/install-driver.ps1 DESTINATION tools)
endif()

if(NOT SSD_CACHE_DEVELOPMENT_USER_MODE_ONLY)
  install(
    FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/build/driver/$<CONFIG>/cachemon.sys"
      "${CMAKE_CURRENT_SOURCE_DIR}/build/driver/$<CONFIG>/cachemon.inf"
    DESTINATION driver
  )
endif()

if(SSD_CACHE_BUILD_INSTALLER)
  set(CPACK_PACKAGE_NAME "ssd-cache")
  set(CPACK_GENERATOR WIX)
  set(CPACK_PACKAGE_FILE_NAME "ssd-cache-${PROJECT_VERSION}-windows-x64")
  set(CPACK_PACKAGE_VENDOR "ssd-cache")
  set(CPACK_PACKAGE_CONTACT "maintainers@example.invalid")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "ssd-cache")
  set(CPACK_WIX_VERSION 4)
  set(CPACK_WIX_ARCHITECTURE x64)
  set(CPACK_WIX_UPGRADE_GUID "7F098F35-A9F5-4C15-955A-D67438D5E934")
  include(CPack)
endif()
