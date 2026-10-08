cmake_minimum_required(VERSION 3.16)
project(disk-manager VERSION 1.01 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_package(Curses REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
find_package(PkgConfig)

option(BUILD_GUI "Build the gtkmm desktop GUI (requires libgtkmm-3.0-dev)" ON)
set(HAVE_GTKMM FALSE)
if(BUILD_GUI AND PkgConfig_FOUND)
  pkg_check_modules(GTKMM gtkmm-3.0)
  if(GTKMM_FOUND)
    set(HAVE_GTKMM TRUE)
  else()
    message(WARNING "libgtkmm-3.0-dev not found -- skipping disk-manager-gui (TUI will still build)")
  endif()
endif()

add_library(dm_core STATIC
  src/core/command.cpp
  src/core/device.cpp
  src/core/smart.cpp
  src/core/partition.cpp
  src/core/filesystem.cpp
  src/core/mdadm.cpp
  src/core/locate.cpp
  src/core/firmware.cpp
  src/core/recovery.cpp
  src/core/power.cpp
)
target_include_directories(dm_core PUBLIC include)
target_link_libraries(dm_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json)

add_executable(disk-manager-tui
  src/tui/main.cpp
  src/tui/tui_app.cpp
  src/tui/ui_helpers.cpp
)
target_include_directories(disk-manager-tui PRIVATE ${CURSES_INCLUDE_DIR})
target_link_libraries(disk-manager-tui PRIVATE dm_core ${CURSES_LIBRARIES})
target_compile_definitions(disk-manager-tui PRIVATE DM_VERSION="${PROJECT_VERSION}")

if(HAVE_GTKMM)
  add_executable(disk-manager-gui
    src/gui/main.cpp
    src/gui/gui_app.cpp
    src/gui/gui_disks.cpp
    src/gui/gui_arrays.cpp
    src/gui/gui_firmware.cpp
    src/gui/gui_recovery.cpp
  )
  target_include_directories(disk-manager-gui PRIVATE ${GTKMM_INCLUDE_DIRS})
  target_link_directories(disk-manager-gui PRIVATE ${GTKMM_LIBRARY_DIRS})
  target_link_libraries(disk-manager-gui PRIVATE dm_core ${GTKMM_LIBRARIES})
  target_compile_options(disk-manager-gui PRIVATE ${GTKMM_CFLAGS_OTHER})
  target_compile_definitions(disk-manager-gui PRIVATE DM_VERSION="${PROJECT_VERSION}")
endif()

install(TARGETS disk-manager-tui RUNTIME DESTINATION bin)
install(FILES data/disk-manager-tui.desktop DESTINATION share/applications)
if(HAVE_GTKMM)
  install(TARGETS disk-manager-gui RUNTIME DESTINATION bin)
  install(FILES data/disk-manager-gui.desktop DESTINATION share/applications)
  install(PROGRAMS data/disk-manager-gui-launch.sh
          DESTINATION bin
          RENAME disk-manager-gui-launch)
endif()

install(FILES data/icons/disk-manager.svg DESTINATION share/icons/hicolor/scalable/apps)
foreach(iconsize 16 22 24 32 48 64 128 256 512)
  install(FILES data/icons/${iconsize}/disk-manager.png
          DESTINATION share/icons/hicolor/${iconsize}x${iconsize}/apps)
endforeach()

# ---------------------------------------------------------------- packaging
set(CPACK_PACKAGE_NAME "disk-manager")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Jesse")
set(CPACK_PACKAGE_CONTACT "Jesse <figjam217@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Disk & RAID manager (ncurses TUI + gtkmm GUI) wrapping mdadm, smartctl, parted, fwupdmgr, ddrescue/testdisk")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
# Runtime tools the core library shells out to -- not linked libraries, so
# shlibdeps can't discover these; declared by hand instead.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "mdadm, smartmontools, parted, e2fsprogs, xfsprogs, btrfs-progs, dosfstools, sg3-utils, ledmon, gddrescue, testdisk, fwupd, hdparm")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/packaging/postinst;${CMAKE_SOURCE_DIR}/packaging/postrm")

include(CPack)
