# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

# QtWebEngine Linux webview backend, built into the upstream lib_webview target.
option(DESKTOP_APP_USE_QTWEBENGINE "Use the QtWebEngine webview backend instead of WebKitGTK." OFF)

if (NOT LINUX OR NOT DESKTOP_APP_USE_QTWEBENGINE)
    return()
endif()

# GLOBAL: lib_webview lives in another directory and must see these targets.
find_package(Qt6 REQUIRED COMPONENTS WebEngineWidgets WebEngineCore WebChannel GLOBAL)

set(webview_qtwebengine_loc ${src_loc}/platform/linux/webview_qtwebengine)
target_sources(lib_webview
PRIVATE
    ${webview_qtwebengine_loc}/webview_linux_qtwebengine.cpp
    ${webview_qtwebengine_loc}/webview_linux_qtwebengine.h
)

target_link_libraries(lib_webview
PRIVATE
    Qt6::WebEngineWidgets
    Qt6::WebEngineCore
    Qt6::WebChannel
)

target_compile_definitions(lib_webview
INTERFACE
    WEBVIEW_QTWEBENGINE
)

# The bridge object (Webview::QtWebEngine::Bridge) needs moc.
set_target_properties(lib_webview PROPERTIES AUTOMOC ON)

# WHY: source properties are directory scoped, so remove_target_sources from
# here would not reach the lib_webview target; TARGET_DIRECTORY does.
set(webview_linux_loc ${CMAKE_CURRENT_SOURCE_DIR}/lib_webview/webview/platform/linux)
set_source_files_properties(
    ${webview_linux_loc}/webview_linux.cpp
    ${webview_linux_loc}/webview_linux_compositor.cpp
    ${webview_linux_loc}/webview_linux_compositor.h
    ${webview_linux_loc}/webview_linux_webkitgtk_library.cpp
    ${webview_linux_loc}/webview_linux_webkitgtk_library.h
    ${webview_linux_loc}/webview_linux_webkitgtk.cpp
    ${webview_linux_loc}/webview_linux_webkitgtk.h
    TARGET_DIRECTORY lib_webview
    PROPERTIES HEADER_FILE_ONLY TRUE SKIP_AUTOGEN TRUE
)
