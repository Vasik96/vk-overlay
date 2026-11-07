# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/vasik96/Downloads/vk_overlay/build/_deps/vulkan_headers-src"
  "/home/vasik96/Downloads/vk_overlay/build/_deps/vulkan_headers-build"
  "/home/vasik96/Downloads/vk_overlay/build/_deps/vulkan_headers-subbuild/vulkan_headers-populate-prefix"
  "/home/vasik96/Downloads/vk_overlay/build/_deps/vulkan_headers-subbuild/vulkan_headers-populate-prefix/tmp"
  "/home/vasik96/Downloads/vk_overlay/build/_deps/vulkan_headers-subbuild/vulkan_headers-populate-prefix/src/vulkan_headers-populate-stamp"
  "/home/vasik96/Downloads/vk_overlay/build/_deps/vulkan_headers-subbuild/vulkan_headers-populate-prefix/src"
  "/home/vasik96/Downloads/vk_overlay/build/_deps/vulkan_headers-subbuild/vulkan_headers-populate-prefix/src/vulkan_headers-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/vasik96/Downloads/vk_overlay/build/_deps/vulkan_headers-subbuild/vulkan_headers-populate-prefix/src/vulkan_headers-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/vasik96/Downloads/vk_overlay/build/_deps/vulkan_headers-subbuild/vulkan_headers-populate-prefix/src/vulkan_headers-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
