# gfxterm: a library to display images on the terminal with kitty, sixel, iterm2 protocols and tmux
# Copyright (C) 2026 David Bellot <david.bellot@gmail.com>
# 
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# Download and build Google Test
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Add test executable
add_executable(tests test_main.cpp)
target_link_libraries(tests PRIVATE gtest_main gfxterm)
target_include_directories(tests PRIVATE ${CMAKE_SOURCE_DIR})

# Discover tests
include(GoogleTest)
gtest_discover_tests(tests)
