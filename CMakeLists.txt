cmake_minimum_required(VERSION 3.20)
project(gst_qt_poc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt meta-objects
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-video-1.0)

add_executable(gst_qt_poc src/main.cpp)
target_include_directories(gst_qt_poc PRIVATE ${GST_INCLUDE_DIRS})
target_link_libraries(gst_qt_poc PRIVATE Qt6::Widgets ${GST_LIBRARIES})
target_compile_options(gst_qt_poc PRIVATE ${GST_CFLAGS_OTHER})
