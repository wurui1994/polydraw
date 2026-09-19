cmake_minimum_required(VERSION 3.24)
project(polydraw_modern_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(POLYDRAW_ENABLE_LLVM "Enable LLVM ORC JIT support when LLVM is available" ON)
set(POLYDRAW_LLVM_ENABLED OFF)

add_library(polydraw_core
  src/parser/sections.c
  src/runtime/host_subset.c
  src/render/render.c
  src/render/glfw_backend.c
  src/render/cgl_backend.c
  src/render/compat.c
  src/render/shader.c
  src/jit/jit_stub.c
)

target_include_directories(polydraw_core PUBLIC include)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GLFW3 QUIET glfw3)
endif()
find_package(JPEG QUIET)
find_package(PNG QUIET)

if(POLYDRAW_ENABLE_LLVM)
  find_package(LLVM QUIET CONFIG)
  find_program(LLVM_CONFIG_EXECUTABLE
    NAMES llvm-config
    PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
    NO_DEFAULT_PATH
  )
  if(NOT LLVM_CONFIG_EXECUTABLE)
    find_program(LLVM_CONFIG_EXECUTABLE llvm-config)
  endif()
  if(LLVM_FOUND)
    set(POLYDRAW_LLVM_ENABLED ON)
    target_compile_definitions(polydraw_core PRIVATE POLYDRAW_WITH_LLVM=1)
    target_compile_definitions(polydraw_core PRIVATE POLYDRAW_LLVM_VERSION="${LLVM_PACKAGE_VERSION}")
    target_include_directories(polydraw_core PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(polydraw_core PRIVATE ${LLVM_DEFINITIONS})
    llvm_map_components_to_libnames(POLYDRAW_LLVM_LIBS core executionengine mcjit native nativecodegen orcjit)
    target_link_libraries(polydraw_core PUBLIC ${POLYDRAW_LLVM_LIBS})
  elseif(LLVM_CONFIG_EXECUTABLE)
    execute_process(
      COMMAND ${LLVM_CONFIG_EXECUTABLE} --version
      OUTPUT_VARIABLE POLYDRAW_LLVM_CONFIG_VERSION
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE POLYDRAW_LLVM_VERSION_RESULT
    )
    execute_process(
      COMMAND ${LLVM_CONFIG_EXECUTABLE} --cflags
      OUTPUT_VARIABLE POLYDRAW_LLVM_CFLAGS
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE POLYDRAW_LLVM_CFLAGS_RESULT
    )
    execute_process(
      COMMAND ${LLVM_CONFIG_EXECUTABLE} --ldflags
      OUTPUT_VARIABLE POLYDRAW_LLVM_LDFLAGS
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE POLYDRAW_LLVM_LDFLAGS_RESULT
    )
    execute_process(
      COMMAND ${LLVM_CONFIG_EXECUTABLE} --libs core orcjit native nativecodegen
      OUTPUT_VARIABLE POLYDRAW_LLVM_LIBS
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE POLYDRAW_LLVM_LIBS_RESULT
    )
    if(POLYDRAW_LLVM_VERSION_RESULT EQUAL 0 AND POLYDRAW_LLVM_CFLAGS_RESULT EQUAL 0 AND POLYDRAW_LLVM_LDFLAGS_RESULT EQUAL 0 AND POLYDRAW_LLVM_LIBS_RESULT EQUAL 0)
      set(POLYDRAW_LLVM_ENABLED ON)
      target_compile_definitions(polydraw_core PRIVATE POLYDRAW_WITH_LLVM=1 POLYDRAW_LLVM_VERSION="${POLYDRAW_LLVM_CONFIG_VERSION}")
      separate_arguments(POLYDRAW_LLVM_CFLAGS_LIST NATIVE_COMMAND "${POLYDRAW_LLVM_CFLAGS}")
      separate_arguments(POLYDRAW_LLVM_LDFLAGS_LIST NATIVE_COMMAND "${POLYDRAW_LLVM_LDFLAGS}")
      separate_arguments(POLYDRAW_LLVM_LIBS_LIST NATIVE_COMMAND "${POLYDRAW_LLVM_LIBS}")
      target_compile_options(polydraw_core PRIVATE ${POLYDRAW_LLVM_CFLAGS_LIST})
      target_link_options(polydraw_core PUBLIC ${POLYDRAW_LLVM_LDFLAGS_LIST})
      target_link_libraries(polydraw_core PUBLIC ${POLYDRAW_LLVM_LIBS_LIST})
    endif()
  endif()
endif()

if(GLFW3_FOUND)
  target_compile_definitions(polydraw_core PRIVATE POLYDRAW_WITH_GLFW=1)
  if(APPLE)
    target_compile_definitions(polydraw_core PRIVATE GL_SILENCE_DEPRECATION=1)
  endif()
  target_include_directories(polydraw_core PRIVATE ${GLFW3_INCLUDE_DIRS})
  target_link_libraries(polydraw_core PUBLIC ${GLFW3_LIBRARIES})
  target_link_directories(polydraw_core PUBLIC ${GLFW3_LIBRARY_DIRS})
  find_package(OpenGL REQUIRED)
  target_link_libraries(polydraw_core PUBLIC OpenGL::GL)
endif()

if(APPLE)
  target_compile_definitions(polydraw_core PRIVATE POLYDRAW_WITH_CGL=1 GL_SILENCE_DEPRECATION=1)
  find_library(COCOA_FRAMEWORK Cocoa)
  find_library(OPENGL_FRAMEWORK OpenGL)
  target_link_libraries(polydraw_core PUBLIC ${OPENGL_FRAMEWORK} ${COCOA_FRAMEWORK})
endif()

if(JPEG_FOUND)
  target_compile_definitions(polydraw_core PRIVATE POLYDRAW_WITH_JPEG=1)
  target_link_libraries(polydraw_core PUBLIC JPEG::JPEG)
endif()

if(PNG_FOUND)
  target_compile_definitions(polydraw_core PRIVATE POLYDRAW_WITH_PNG=1)
  target_link_libraries(polydraw_core PUBLIC PNG::PNG)
endif()

add_executable(polydrawc
  src/cli/polydrawc.c
)

target_link_libraries(polydrawc PRIVATE polydraw_core)
target_compile_definitions(polydrawc PRIVATE
  POLYDRAW_JS_CLI="${CMAKE_CURRENT_SOURCE_DIR}/../js/src/cli/polydraw-js.js"
)

find_program(NODE_EXECUTABLE node)
find_package(Python3 COMPONENTS Interpreter QUIET)

enable_testing()
add_test(
  NAME compat_smoke_ppm
  COMMAND polydrawc compat-smoke ${CMAKE_CURRENT_BINARY_DIR}/compat_smoke.ppm
)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME compat_smoke_ppm_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/compat_smoke.ppm
  )
  set_tests_properties(compat_smoke_ppm_check PROPERTIES DEPENDS compat_smoke_ppm)
endif()
add_test(NAME sections_basic COMMAND polydrawc sections ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/host_basic.pss)
add_test(NAME c_native_trace_balls COMMAND polydrawc trace-native-batches ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/balls.pss)
set_tests_properties(c_native_trace_balls PROPERTIES PASS_REGULAR_EXPRESSION "^batch 4 49152")
add_test(NAME c_cli_run_host_basic COMMAND polydrawc run ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/host_basic.pss)
set_tests_properties(c_cli_run_host_basic PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":14")
add_test(NAME c_subset_math_logic COMMAND polydrawc run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/host_math_logic.pss)
set_tests_properties(c_subset_math_logic PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":37")
add_test(NAME c_subset_functions_goto COMMAND polydrawc run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/functions_goto.pss)
set_tests_properties(c_subset_functions_goto PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":15")
add_test(NAME c_subset_reference_params COMMAND polydrawc run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/reference_params.pss)
set_tests_properties(c_subset_reference_params PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":7")
add_test(NAME c_subset_control_flow COMMAND polydrawc run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/control_flow.pss)
set_tests_properties(c_subset_control_flow PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":12")
add_test(NAME c_subset_if_else_blocks COMMAND polydrawc run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/if_else_blocks.pss)
set_tests_properties(c_subset_if_else_blocks PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":69")
add_test(NAME c_subset_do_while COMMAND polydrawc run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/do_while.pss)
set_tests_properties(c_subset_do_while PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":9")
add_test(NAME c_subset_array_params COMMAND polydrawc run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/array_params.pss)
set_tests_properties(c_subset_array_params PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":48")
add_test(NAME c_subset_klock COMMAND polydrawc run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/klock.pss)
set_tests_properties(c_subset_klock PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":1")
add_test(NAME c_subset_glklock COMMAND polydrawc run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/glklock.pss)
set_tests_properties(c_subset_glklock PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":1")
add_test(
  NAME render_pss_texture_host
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/texture.pss
    ${CMAKE_CURRENT_BINARY_DIR}/texture_host.ppm
)
add_test(
  NAME render_uniform_color
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/uniform_color.pss
    ${CMAKE_CURRENT_BINARY_DIR}/uniform_color.ppm
)
add_test(
  NAME render_native_immediate
  COMMAND polydrawc render-pss-native
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/webgl_immediate.pss
    ${CMAKE_CURRENT_BINARY_DIR}/native_immediate.ppm
)
add_test(
  NAME render_native_uniform_color
  COMMAND polydrawc render-pss-native
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/uniform_color.pss
    ${CMAKE_CURRENT_BINARY_DIR}/native_uniform_color.ppm
)
add_test(
  NAME render_texture_binding
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/texture_binding.pss
    ${CMAKE_CURRENT_BINARY_DIR}/texture_binding.ppm
)
add_test(
  NAME render_texture_upload_float
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/texture_upload_float.pss
    ${CMAKE_CURRENT_BINARY_DIR}/texture_upload_float.ppm
)
add_test(
  NAME render_texture_file_ppm
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/texture_file_ppm.pss
    ${CMAKE_CURRENT_BINARY_DIR}/texture_file_ppm.ppm
)
add_test(
  NAME render_texture_file_jpeg
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/texture_file_jpeg.pss
    ${CMAKE_CURRENT_BINARY_DIR}/texture_file_jpeg.ppm
)
add_test(
  NAME render_texture_file_png
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/texture_file_png.pss
    ${CMAKE_CURRENT_BINARY_DIR}/texture_file_png.ppm
)
add_test(
  NAME render_capture_texture
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/capture_texture.pss
    ${CMAKE_CURRENT_BINARY_DIR}/capture_texture.ppm
)
add_test(
  NAME render_matrix_transform
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/matrix_transform.pss
    ${CMAKE_CURRENT_BINARY_DIR}/matrix_transform.ppm
)
add_test(
  NAME render_drawcone2_limited
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/drawcone2.pss
    ${CMAKE_CURRENT_BINARY_DIR}/drawcone2_limited.ppm
)
set_tests_properties(render_drawcone2_limited PROPERTIES ENVIRONMENT "POLYDRAW_BATCH_LIMIT=64")
add_test(
  NAME render_geo_test
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/geo_test.pss
    ${CMAKE_CURRENT_BINARY_DIR}/geo_test.ppm
)
add_test(
  NAME render_geo_duptris
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/geo_duptris.pss
    ${CMAKE_CURRENT_BINARY_DIR}/geo_duptris.ppm
)
add_test(
  NAME render_cubetex
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/cubetex.pss
    ${CMAKE_CURRENT_BINARY_DIR}/cubetex.ppm
)
add_test(
  NAME render_texture3d
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/texture3d.pss
    ${CMAKE_CURRENT_BINARY_DIR}/texture3d.ppm
)
add_test(
  NAME render_disco_ball_limited
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tigrou/disco\ ball.pss
    ${CMAKE_CURRENT_BINARY_DIR}/disco_ball_limited.ppm
)
set_tests_properties(render_disco_ball_limited PROPERTIES ENVIRONMENT "POLYDRAW_BATCH_LIMIT=32")
add_test(
  NAME render_ribbons_invasion_warmup
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tigrou/ribbons\ invasion.pss
    ${CMAKE_CURRENT_BINARY_DIR}/ribbons_invasion_warmup.ppm
)
set_tests_properties(render_ribbons_invasion_warmup PROPERTIES ENVIRONMENT "POLYDRAW_FRAMES=60;POLYDRAW_BATCH_LIMIT=32")
add_test(
  NAME render_dominos_camera
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tigrou/dominos.pss
    ${CMAKE_CURRENT_BINARY_DIR}/dominos_camera.ppm
)
set_tests_properties(render_dominos_camera PROPERTIES ENVIRONMENT "POLYDRAW_BATCH_LIMIT=32")
add_test(
  NAME render_tree_capture
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tigrou/tree.pss
    ${CMAKE_CURRENT_BINARY_DIR}/tree_capture.ppm
)
add_test(
  NAME render_interference_asm
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/interference_asm.pss
    ${CMAKE_CURRENT_BINARY_DIR}/interference_asm.ppm
)
add_test(
  NAME render_multiarb_asm
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/multiarb_asm.pss
    ${CMAKE_CURRENT_BINARY_DIR}/multiarb_asm.ppm
)
add_test(
  NAME render_creepers_asm
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/creepers_asm.pss
    ${CMAKE_CURRENT_BINARY_DIR}/creepers_asm.ppm
)
add_test(
  NAME render_drawsph_asm
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/drawsph_asm.pss
    ${CMAKE_CURRENT_BINARY_DIR}/drawsph_asm.ppm
)
add_test(
  NAME render_drawcone2_asm_limited
  COMMAND polydrawc render-pss
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/drawcone2_asm.pss
    ${CMAKE_CURRENT_BINARY_DIR}/drawcone2_asm_limited.ppm
)
set_tests_properties(render_drawcone2_asm_limited PROPERTIES ENVIRONMENT "POLYDRAW_BATCH_LIMIT=192")
if(GLFW3_FOUND OR APPLE)
  add_test(NAME shader_compile_balls COMMAND polydrawc compile-shaders ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/balls.pss)
  add_test(
    NAME render_gradient_ppm
    COMMAND polydrawc render-quad
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/render_gradient.pss
      ${CMAKE_CURRENT_BINARY_DIR}/render_gradient.ppm
  )
endif()

if(POLYDRAW_LLVM_ENABLED)
  add_test(NAME jit_smoke COMMAND polydrawc jit-smoke)
  set_tests_properties(jit_smoke PROPERTIES PASS_REGULAR_EXPRESSION "\"value\":42")
  add_test(NAME jit_run_subset_host_basic COMMAND polydrawc jit-run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/host_basic.pss)
  set_tests_properties(jit_run_subset_host_basic PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":14")
  add_test(NAME jit_run_subset_math_logic COMMAND polydrawc jit-run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/host_math_logic.pss)
  set_tests_properties(jit_run_subset_math_logic PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":37")
  add_test(NAME jit_run_subset_functions_goto COMMAND polydrawc jit-run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/functions_goto.pss)
  set_tests_properties(jit_run_subset_functions_goto PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":15")
  add_test(NAME jit_run_subset_reference_params COMMAND polydrawc jit-run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/reference_params.pss)
  set_tests_properties(jit_run_subset_reference_params PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":7")
  add_test(NAME jit_run_subset_control_flow COMMAND polydrawc jit-run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/control_flow.pss)
  set_tests_properties(jit_run_subset_control_flow PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":12")
  add_test(NAME jit_run_subset_if_else_blocks COMMAND polydrawc jit-run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/if_else_blocks.pss)
  set_tests_properties(jit_run_subset_if_else_blocks PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":69")
  add_test(NAME jit_run_subset_do_while COMMAND polydrawc jit-run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/do_while.pss)
  set_tests_properties(jit_run_subset_do_while PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":9")
  add_test(NAME jit_run_subset_array_params COMMAND polydrawc jit-run-subset ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/array_params.pss)
  set_tests_properties(jit_run_subset_array_params PROPERTIES PASS_REGULAR_EXPRESSION "\"return\":48")
endif()

if(NODE_EXECUTABLE)
  add_test(
    NAME js_host_conformance
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../js/src/cli/conformance.js
  )
  add_test(
    NAME js_trace_capture_metadata
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../js/src/cli/polydraw-js.js
      trace-batches ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/capture_texture.pss
      --step-limit=100000 --batch-limit=8
  )
  set_tests_properties(js_trace_capture_metadata PROPERTIES PASS_REGULAR_EXPRESSION "^texcapture 8")
  add_test(
    NAME js_trace_gspiral_enum_shadow_scope
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../js/src/cli/polydraw-js.js
      trace-batches ${CMAKE_CURRENT_SOURCE_DIR}/../../ken/gspiral.pss
      --step-limit=50000000 --batch-limit=1
  )
  set_tests_properties(js_trace_gspiral_enum_shadow_scope PROPERTIES PASS_REGULAR_EXPRESSION "^batch 6 13")
  add_test(
    NAME js_trace_xres_yres_options
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../js/src/cli/polydraw-js.js
      trace-batches ${CMAKE_CURRENT_SOURCE_DIR}/../shared/fixtures/xres_yres_color.pss
      --xres=1280 --yres=960
  )
  set_tests_properties(js_trace_xres_yres_options PROPERTIES PASS_REGULAR_EXPRESSION "0\\.333333333 0\\.750000000")
endif()

if((GLFW3_FOUND OR APPLE) AND Python3_Interpreter_FOUND)
  add_test(
    NAME render_gradient_ppm_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/render_gradient.ppm
  )
  set_tests_properties(render_gradient_ppm_check PROPERTIES DEPENDS render_gradient_ppm)
endif()

if(Python3_Interpreter_FOUND)
  add_test(
    NAME render_pss_texture_host_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/texture_host.ppm
  )
  set_tests_properties(render_pss_texture_host_check PROPERTIES DEPENDS render_pss_texture_host)
  add_test(
    NAME render_uniform_color_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/uniform_color.ppm
  )
  set_tests_properties(render_uniform_color_check PROPERTIES DEPENDS render_uniform_color)
  add_test(
    NAME render_native_immediate_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/native_immediate.ppm
  )
  set_tests_properties(render_native_immediate_check PROPERTIES DEPENDS render_native_immediate)
  add_test(
    NAME render_native_uniform_color_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/native_uniform_color.ppm
  )
  set_tests_properties(render_native_uniform_color_check PROPERTIES DEPENDS render_native_uniform_color)
  add_test(
    NAME render_texture_binding_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/texture_binding.ppm
  )
  set_tests_properties(render_texture_binding_check PROPERTIES DEPENDS render_texture_binding)
  add_test(
    NAME render_texture_upload_float_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/texture_upload_float.ppm
  )
  set_tests_properties(render_texture_upload_float_check PROPERTIES DEPENDS render_texture_upload_float)
  add_test(
    NAME render_texture_file_ppm_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/texture_file_ppm.ppm
  )
  set_tests_properties(render_texture_file_ppm_check PROPERTIES DEPENDS render_texture_file_ppm)
  add_test(
    NAME render_texture_file_jpeg_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/texture_file_jpeg.ppm
  )
  set_tests_properties(render_texture_file_jpeg_check PROPERTIES DEPENDS render_texture_file_jpeg)
  add_test(
    NAME render_texture_file_png_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/texture_file_png.ppm
  )
  set_tests_properties(render_texture_file_png_check PROPERTIES DEPENDS render_texture_file_png)
  add_test(
    NAME render_capture_texture_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/capture_texture.ppm
  )
  set_tests_properties(render_capture_texture_check PROPERTIES DEPENDS render_capture_texture)
  add_test(
    NAME render_matrix_transform_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/matrix_transform.ppm
  )
  set_tests_properties(render_matrix_transform_check PROPERTIES DEPENDS render_matrix_transform)
  add_test(
    NAME render_drawcone2_limited_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/drawcone2_limited.ppm
  )
  set_tests_properties(render_drawcone2_limited_check PROPERTIES DEPENDS render_drawcone2_limited)
  add_test(
    NAME render_geo_test_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/geo_test.ppm
  )
  set_tests_properties(render_geo_test_check PROPERTIES DEPENDS render_geo_test)
  add_test(
    NAME render_geo_duptris_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/geo_duptris.ppm
  )
  set_tests_properties(render_geo_duptris_check PROPERTIES DEPENDS render_geo_duptris)
  add_test(
    NAME render_cubetex_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/cubetex.ppm
  )
  set_tests_properties(render_cubetex_check PROPERTIES DEPENDS render_cubetex)
  add_test(
    NAME render_texture3d_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/texture3d.ppm
  )
  set_tests_properties(render_texture3d_check PROPERTIES DEPENDS render_texture3d)
  add_test(
    NAME render_disco_ball_limited_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/disco_ball_limited.ppm
  )
  set_tests_properties(render_disco_ball_limited_check PROPERTIES DEPENDS render_disco_ball_limited)
  add_test(
    NAME render_ribbons_invasion_warmup_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/ribbons_invasion_warmup.ppm
  )
  set_tests_properties(render_ribbons_invasion_warmup_check PROPERTIES DEPENDS render_ribbons_invasion_warmup)
  add_test(
    NAME render_dominos_camera_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/dominos_camera.ppm
  )
  set_tests_properties(render_dominos_camera_check PROPERTIES DEPENDS render_dominos_camera)
  add_test(
    NAME render_tree_capture_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/tree_capture.ppm
  )
  set_tests_properties(render_tree_capture_check PROPERTIES DEPENDS render_tree_capture)
  add_test(
    NAME render_interference_asm_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/interference_asm.ppm
  )
  set_tests_properties(render_interference_asm_check PROPERTIES DEPENDS render_interference_asm)
  add_test(
    NAME render_multiarb_asm_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/multiarb_asm.ppm
  )
  set_tests_properties(render_multiarb_asm_check PROPERTIES DEPENDS render_multiarb_asm)
  add_test(
    NAME render_creepers_asm_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/creepers_asm.ppm
  )
  set_tests_properties(render_creepers_asm_check PROPERTIES DEPENDS render_creepers_asm)
  add_test(
    NAME render_drawsph_asm_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/drawsph_asm.ppm
  )
  set_tests_properties(render_drawsph_asm_check PROPERTIES DEPENDS render_drawsph_asm)
  add_test(
    NAME render_drawcone2_asm_limited_check
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../shared/tools/check_ppm.py
      ${CMAKE_CURRENT_BINARY_DIR}/drawcone2_asm_limited.ppm
  )
  set_tests_properties(render_drawcone2_asm_limited_check PROPERTIES DEPENDS render_drawcone2_asm_limited)
endif()
