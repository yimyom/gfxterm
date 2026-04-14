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

cmake_minimum_required(VERSION 3.12)
project(gfxterm)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build shared library
add_library(gfxterm SHARED gfxterm.cpp)

# Set library output name
set_target_properties(gfxterm PROPERTIES
    VERSION 0.5.0
    SOVERSION 1
)

# Include directories for library users
target_include_directories(gfxterm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Enable testing
enable_testing()

# Add test subdirectory
add_subdirectory(test)
