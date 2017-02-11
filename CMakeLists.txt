cmake_minimum_required(VERSION 3.5)

set(n "qhttp")

project(${n})

find_package(Qt5Core)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

add_definitions(-DQHTTP_MEMORY_LOG=0)
add_definitions(-DQHTTP_HAS_CLIENT)
# win32
#add_definitions(-DQHTTP_EXPORT)

add_library(${n}
  src/qhttpabstracts.cpp
  src/qhttpserverconnection.cpp
  src/qhttpserverrequest.cpp
  src/qhttpserverresponse.cpp
  src/qhttpserver.cpp

  src/qhttpclientrequest.cpp
  src/qhttpclientresponse.cpp
  src/qhttpclient.cpp
)

set(public_headers
  src/qhttpfwd.hpp
  src/qhttpabstracts.hpp
  src/qhttpserverconnection.hpp
  src/qhttpserverrequest.hpp
  src/qhttpserverresponse.hpp
  src/qhttpserver.hpp
    
  src/qhttpclient.hpp
  src/qhttpclientresponse.hpp
  src/qhttpclientrequest.hpp
)

target_include_directories(${n} PUBLIC src)

target_link_libraries(${n}
  http-parser
  ${Qt5Core_QTMAIN_LIBRARIES}
)

qt5_use_modules(${n} Core Network)

set_target_properties(${n} PROPERTIES PUBLIC_HEADER ${public_headers})



