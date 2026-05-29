cmake_minimum_required(VERSION 3.16)
project(market_data_ingestion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(market_data_app main.cpp)
target_compile_options(market_data_app PRIVATE -O3 -Wall -Wextra -pedantic)
target_link_libraries(market_data_app PRIVATE pthread)
