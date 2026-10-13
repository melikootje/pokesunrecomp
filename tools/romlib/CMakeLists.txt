add_library(romlib STATIC
  src/common.cpp src/sha256.cpp src/blz.cpp src/ncch.cpp src/exefs.cpp src/romfs.cpp
  src/profile.cpp src/dump.cpp)
target_include_directories(romlib PUBLIC include)
