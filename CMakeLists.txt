cmake_minimum_required(VERSION 3.16)

project(MinPlay VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets LinguistTools)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets LinguistTools)
find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libavdevice libswscale libswresample)

set(TS_FILES MinPlay_en_US.ts)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.hpp


        src/GUI/CDockWidget.hpp
        src/GUI/CDockWidget.cpp
        src/GUI/VideoDock.hpp
        src/GUI/VideoDock.cpp
        src/GUI/DisplayWidget.hpp
        src/GUI/DisplayWidget.cpp
        src/GUI/MenuBarMenu.hpp
        src/GUI/MenuBarMenu.cpp
        src/GUI/MenuBar.hpp
        src/GUI/MenuBar.cpp
        src/GUI/ToolBar.hpp
        src/GUI/ToolBar.cpp


        playback/cavpacket.hpp
        playback/cavpacket.cpp


        ${TS_FILES}
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(MinPlay
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        playback/cavframe.h playback/cavframe.cpp
        playback/cavchannellayout.hpp playback/cavchannellayout.cpp
        playback/csubtitle.hpp playback/csubtitle.cpp
        playback/sdlrenderer.hpp playback/sdlrenderer.cpp

        src/GUI/videodisplaywidget.hpp src/GUI/videodisplaywidget.cpp
        playback/playbackengine.hpp playback/playbackengine.cpp


        playback/cavstream.hpp playback/cavstream.cpp
        playback/sdlkeymap.cpp
        playback/sdlkeymap.hpp
        src/GUI/StatusBar.hpp src/GUI/StatusBar.cpp
        src/GUI/Slider.hpp src/GUI/Slider.cpp
        src/utils.hpp
        src/utils.cpp
        src/GUI/AppEventFilter.hpp src/GUI/AppEventFilter.cpp
        src/GUI/LoggerWidget.h src/GUI/LoggerWidget.cpp
        src/GUI/Playlist.hpp src/GUI/Playlist.cpp
        src/GUI/InfoWidget.h src/GUI/InfoWidget.cpp
        playback/formatcontext.hpp playback/formatcontext.cpp
        playback/avframeview.hpp playback/avframeview.cpp
        playback/packetqueue.hpp playback/packetqueue.cpp



    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET MinPlay APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation

    qt_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})
else()
    if(ANDROID)
        add_library(MinPlay SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(MinPlay
            ${PROJECT_SOURCES}
        )
    endif()

    qt5_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})
endif()

target_link_libraries(MinPlay PRIVATE Qt${QT_VERSION_MAJOR}::Widgets
    PRIVATE SDL3::SDL3
    PkgConfig::FFMPEG
)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.MinPlay)
endif()
set_target_properties(MinPlay PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS MinPlay
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(MinPlay)
endif()
