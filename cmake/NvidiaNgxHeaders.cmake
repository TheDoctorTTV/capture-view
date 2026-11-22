# Fetches the NVIDIA NGX (DLSS) SDK build inputs from NVIDIA's official GitHub
# repository at configure time: the API headers and the small NGX loader
# library (libnvsdk_ngx.a) that NVIDIA requires Linux applications to link.
# Nothing from NVIDIA is committed to this repository. The large DLSS Frame
# Generation runtime (libnvidia-ngx-dlssg.so) is never bundled: users download
# it from NVIDIA from inside the app, and the NGX core is part of the driver.
#
# Every file is pinned to one commit (DLSS SDK v310.9.16) and verified by
# SHA-256, so a changed upstream file fails the build instead of being used.

set(CAPTURE_VIEW_NGX_COMMIT "9981c6350dc58c0ce21f024fd65e31e851c31689")
set(CAPTURE_VIEW_NGX_BASE_URL "https://raw.githubusercontent.com/NVIDIA/DLSS/${CAPTURE_VIEW_NGX_COMMIT}")
set(CAPTURE_VIEW_NGX_DIR "${CMAKE_BINARY_DIR}/third-party/nvidia-ngx")
set(CAPTURE_VIEW_NGX_INCLUDE_DIR "${CAPTURE_VIEW_NGX_DIR}/include")
set(CAPTURE_VIEW_NGX_LIBRARY "${CAPTURE_VIEW_NGX_DIR}/lib/Linux_x86_64/libnvsdk_ngx.a")

set(CAPTURE_VIEW_NGX_FILES
  "include/nvsdk_ngx.h=dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"
  "include/nvsdk_ngx_defs.h=ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"
  "include/nvsdk_ngx_defs_dlssg.h=c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1"
  "include/nvsdk_ngx_defs_vk.h=0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031"
  "include/nvsdk_ngx_helpers_vk.h=c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623"
  "include/nvsdk_ngx_helpers_dlssg_vk.h=b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316"
  "include/nvsdk_ngx_params.h=943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"
  "include/nvsdk_ngx_params_dlssg.h=c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad"
  "include/nvsdk_ngx_vk.h=2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d"
  "lib/Linux_x86_64/libnvsdk_ngx.a=d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df"
  "LICENSE.txt=d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"
)

function(capture_view_fetch_ngx_sdk out_ok)
  foreach(entry IN LISTS CAPTURE_VIEW_NGX_FILES)
    string(REPLACE "=" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 hash)
    set(destination "${CAPTURE_VIEW_NGX_DIR}/${name}")
    get_filename_component(parent "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    if(EXISTS "${destination}")
      file(SHA256 "${destination}" existing)
      if(existing STREQUAL hash)
        continue()
      endif()
      file(REMOVE "${destination}")
    endif()
    message(STATUS "Downloading NVIDIA NGX SDK file ${name}")
    file(DOWNLOAD "${CAPTURE_VIEW_NGX_BASE_URL}/${name}" "${destination}.part"
      EXPECTED_HASH SHA256=${hash}
      TLS_VERIFY ON
      STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      list(GET status 1 reason)
      file(REMOVE "${destination}.part")
      message(WARNING "Could not download NVIDIA NGX SDK file ${name}: ${reason}. "
                      "Frame generation will be disabled in this build.")
      set(${out_ok} OFF PARENT_SCOPE)
      return()
    endif()
    file(RENAME "${destination}.part" "${destination}")
  endforeach()
  set(${out_ok} ON PARENT_SCOPE)
endfunction()
