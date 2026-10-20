cmake_minimum_required(VERSION 3.16)
project(PBRifyCoreTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

set(PBRIFY_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Source/PBRify/Private/Processing)

add_executable(PBRifyCoreTests
	PBRifyCoreTests.cpp
	${PBRIFY_CORE_DIR}/PBRifyCore.cpp
)
target_include_directories(PBRifyCoreTests PRIVATE ${PBRIFY_CORE_DIR})
target_link_libraries(PBRifyCoreTests PRIVATE Threads::Threads)

# Unreal builds with strict warnings (shadowing is an error), so mirror that here.
if(MSVC)
	target_compile_options(PBRifyCoreTests PRIVATE /W4 /WX)
else()
	target_compile_options(PBRifyCoreTests PRIVATE -Wall -Wextra -Wshadow -Werror)
endif()
