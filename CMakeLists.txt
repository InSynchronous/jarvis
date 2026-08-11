cmake_minimum_required(VERSION 3.20)

project(jarvis)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(external/whisper.cpp)
add_subdirectory(external/json)
add_subdirectory(external/FTXUI)
find_package(CURL REQUIRED)

add_executable(jarvis
    src/main.cpp
	src/Tui.cpp
	src/Whisper.cpp
	src/Microphone.cpp
	src/Ollama.cpp
	src/StdioTransport.cpp
	src/HTTPTransport.cpp
	src/MCPConnection.cpp
	src/MCPManager.cpp
	src/SessionManager.cpp
	external/termmark/src/termmark.cpp
	external/termmark/src/parser/MarkdownParser.cpp
	external/termmark/src/renderer/Renderer.cpp
	external/termmark/src/utils/FileIO.cpp
	external/termmark/src/utils/compability.cpp
)

target_include_directories(jarvis PRIVATE
	external/miniaudio
	external/termmark/include
	external/termmark/include/parser
	external/termmark/include/renderer
	external/termmark/include/utils
)

target_link_libraries(jarvis PRIVATE whisper)
target_link_libraries(jarvis PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(jarvis PRIVATE CURL::libcurl)
target_link_libraries(jarvis PRIVATE component)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/scroll_test.cpp")
	add_executable(scroll_test tests/scroll_test.cpp)
	target_link_libraries(scroll_test PRIVATE dom screen)
endif()
