# Compiles GLSL shaders to SPIR-V with glslc and emits them as comma-separated
# uint32 lists ("<name>.spv.inc") that C++ code #includes into arrays.

find_program(CAPTURE_VIEW_GLSLC glslc REQUIRED)

function(capture_view_add_shaders target)
  set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  file(MAKE_DIRECTORY "${output_dir}")
  set(outputs)
  foreach(shader IN LISTS ARGN)
    get_filename_component(name "${shader}" NAME)
    set(output "${output_dir}/${name}.spv.inc")
    add_custom_command(
      OUTPUT "${output}"
      COMMAND "${CAPTURE_VIEW_GLSLC}" --target-env=vulkan1.2 -O -mfmt=num -o "${output}" "${shader}"
      DEPENDS "${shader}"
      COMMENT "Compiling shader ${name}"
      VERBATIM)
    list(APPEND outputs "${output}")
  endforeach()
  add_custom_target(${target}-shaders DEPENDS ${outputs})
  add_dependencies(${target} ${target}-shaders)
  target_include_directories(${target} PRIVATE "${output_dir}")
endfunction()
