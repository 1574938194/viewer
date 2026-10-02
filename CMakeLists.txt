cmake_minimum_required(VERSION 3.24)

project(Viewer VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VCPKG_INSTALLED "D:/vcpkg/installed/x64-windows")

add_executable(vr WIN32
  src/main.cpp
  resources/viewer.rc
  src/app/settings.cpp
  src/util/string_util.cpp
  src/util/tag_values.cpp
  src/util/content_hash.cpp
  src/domain/sample_assets.cpp
  src/database/sqlite_database.cpp
  src/database/migration.cpp
  src/repository/resource_repository.cpp
  src/repository/tag_repository.cpp
  src/repository/category_repository.cpp
  src/repository/actor_repository.cpp
  src/render/theme.cpp
  src/render/renderer.cpp
  src/service/thumbnail_service.cpp
  src/service/playback_service.cpp
  src/service/delete_service.cpp
  src/service/archive_service.cpp
  src/service/import_service.cpp
  src/service/media_probe.cpp
  src/service/search_service.cpp
  src/service/scan_service.cpp
  src/ui/search_bar.cpp
  src/ui/resource_grid.cpp
  src/ui/resource_order.cpp
  src/ui/selection_drag.cpp
  src/ui/delete_dialog.cpp
  src/ui/dark_controls.cpp
  src/ui/import_dialog.cpp
  src/ui/settings_dialog.cpp
  src/app/application.cpp
)

if(MSVC)
  target_compile_options(vr PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_definitions(vr PRIVATE NOMINMAX UNICODE _UNICODE)
endif()

target_include_directories(vr PRIVATE src assets ${VCPKG_INSTALLED}/include)

target_link_libraries(vr PRIVATE
  user32 gdi32 shell32 ole32 oleaut32 advapi32 uuid dwmapi
  d2d1 dwrite windowscodecs shlwapi gdiplus bcrypt
  ${VCPKG_INSTALLED}/lib/sqlite3.lib
  ${VCPKG_INSTALLED}/lib/avformat.lib
  ${VCPKG_INSTALLED}/lib/avcodec.lib
  ${VCPKG_INSTALLED}/lib/avutil.lib
  ${VCPKG_INSTALLED}/lib/swresample.lib
)

add_custom_command(TARGET vr POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${VCPKG_INSTALLED}/bin/sqlite3.dll" $<TARGET_FILE_DIR:vr>)

foreach(ffmpeg_runtime avformat-63.dll avcodec-63.dll avutil-61.dll
                         swresample-7.dll)
  add_custom_command(TARGET vr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VCPKG_INSTALLED}/bin/${ffmpeg_runtime}"
            $<TARGET_FILE_DIR:vr>)
endforeach()

include(CTest)
if(BUILD_TESTING)
  add_executable(nexus_phase2_test
    tests/phase2_integration_test.cpp
    src/app/settings.cpp
    src/util/string_util.cpp
    src/util/content_hash.cpp
    src/database/sqlite_database.cpp
    src/database/migration.cpp
    src/repository/resource_repository.cpp
    src/repository/tag_repository.cpp
    src/repository/category_repository.cpp
    src/repository/actor_repository.cpp
    src/service/archive_service.cpp
    src/service/delete_service.cpp
    src/service/import_service.cpp
    src/service/media_probe.cpp
    src/service/search_service.cpp
    src/service/scan_service.cpp
  )
  target_include_directories(nexus_phase2_test PRIVATE
    src ${VCPKG_INSTALLED}/include)
  target_link_libraries(nexus_phase2_test PRIVATE
    shell32 ole32 windowscodecs bcrypt ${VCPKG_INSTALLED}/lib/sqlite3.lib)
  target_link_libraries(nexus_phase2_test PRIVATE
    ${VCPKG_INSTALLED}/lib/avformat.lib
    ${VCPKG_INSTALLED}/lib/avcodec.lib
    ${VCPKG_INSTALLED}/lib/avutil.lib
    ${VCPKG_INSTALLED}/lib/swresample.lib)
  if(MSVC)
    target_compile_options(nexus_phase2_test PRIVATE
      /W4 /permissive- /utf-8 /EHsc)
    target_compile_definitions(nexus_phase2_test PRIVATE
      NOMINMAX UNICODE _UNICODE)
  endif()
  foreach(ffmpeg_runtime avformat-63.dll avcodec-63.dll avutil-61.dll
                           swresample-7.dll)
    add_custom_command(TARGET nexus_phase2_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${VCPKG_INSTALLED}/bin/${ffmpeg_runtime}"
              $<TARGET_FILE_DIR:nexus_phase2_test>)
  endforeach()
  add_test(NAME nexus_phase2_integration COMMAND nexus_phase2_test)

  get_target_property(nexus_ui_test_sources vr SOURCES)
  list(REMOVE_ITEM nexus_ui_test_sources src/main.cpp)
  add_executable(nexus_ui_test tests/ui_interaction_test.cpp ${nexus_ui_test_sources})
  target_include_directories(nexus_ui_test PRIVATE src assets ${VCPKG_INSTALLED}/include)
  get_target_property(nexus_ui_test_libraries vr LINK_LIBRARIES)
  target_link_libraries(nexus_ui_test PRIVATE ${nexus_ui_test_libraries})
  if(MSVC)
    target_compile_options(nexus_ui_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_compile_definitions(nexus_ui_test PRIVATE NOMINMAX UNICODE _UNICODE)
  endif()
  add_test(NAME nexus_ui_interaction COMMAND nexus_ui_test)
  set_tests_properties(nexus_ui_interaction PROPERTIES TIMEOUT 30)
  add_custom_command(TARGET nexus_ui_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:nexus_ui_test>
            $<TARGET_FILE_DIR:nexus_ui_test>/nexus_ui_preview.exe)
  foreach(ui_test_runtime sqlite3.dll avformat-63.dll avcodec-63.dll
                          avutil-61.dll swresample-7.dll)
    add_custom_command(TARGET nexus_ui_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${VCPKG_INSTALLED}/bin/${ui_test_runtime}"
              $<TARGET_FILE_DIR:nexus_ui_test>)
  endforeach()
  foreach(ui_test_runtime sqlite3.dll avformat-63.dll avcodec-63.dll
                          avutil-61.dll swresample-7.dll)
    add_custom_command(TARGET nexus_ui_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${VCPKG_INSTALLED}/bin/${ui_test_runtime}"
              $<TARGET_FILE_DIR:nexus_ui_test>)
  endforeach()
endif()
