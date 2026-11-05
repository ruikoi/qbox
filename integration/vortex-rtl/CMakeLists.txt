set(VORTEX_SOURCE "${PROJECT_SOURCE_DIR}/../vortex" CACHE PATH "Vortex source tree")
set(VORTEX_RTL_GENERATED "${VORTEX_SOURCE}/build-qbox-rtl/generated" CACHE PATH "Exported Verilator sources")
set(VLT "${VORTEX_RTL_GENERATED}/verilator/include")
file(GLOB MODEL_CPP CONFIGURE_DEPENDS "${VORTEX_RTL_GENERATED}/model/*.cpp")
if(NOT MODEL_CPP)
  message(FATAL_ERROR "Generate and extract Vortex RTL sources first")
endif()
file(GLOB TRACE_CPP "${VORTEX_RTL_GENERATED}/model/*__Trace*.cpp")
set_source_files_properties(${TRACE_CPP} PROPERTIES COMPILE_OPTIONS "-O0")
add_library(vortex_afu_rtl STATIC ${MODEL_CPP} rtl/rtl_time.cc
    "${VLT}/verilated.cpp" "${VLT}/verilated_dpi.cpp"
    "${VLT}/verilated_threads.cpp" "${VLT}/verilated_vcd_c.cpp"
    "${VORTEX_SOURCE}/hw/dpi/dpi_util.cpp" "${VORTEX_SOURCE}/hw/dpi/dpi_float.cpp"
    "${VORTEX_SOURCE}/sw/common/softfloat_ext.cpp" "${VORTEX_SOURCE}/sw/common/rvfloats.cpp")
set_target_properties(vortex_afu_rtl PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(vortex_afu_rtl PUBLIC
    "${PROJECT_SOURCE_DIR}/systemc-components/common/include"
    "${VORTEX_RTL_GENERATED}/model" "${VLT}" "${VLT}/vltstd"
    "${VORTEX_RTL_GENERATED}/config/sw" "${VORTEX_RTL_GENERATED}/config/hw"
    "${VORTEX_SOURCE}/sw/common" "${VORTEX_SOURCE}/third_party/softfloat/source/include")
target_compile_definitions(vortex_afu_rtl PUBLIC VM_TRACE=1 VM_TRACE_VCD=1 VM_TRACE_FST=0
    VX_CFG_XLEN=32 VX_CFG_XLEN_32 THREAD_LOCAL=__thread)
target_compile_options(vortex_afu_rtl PRIVATE -O2)
set(VORTEX_SOFTFLOAT_LIBRARY "${VORTEX_SOURCE}/third_party/softfloat/build/Linux-x86_64-GCC/softfloat.a"
    CACHE FILEPATH "SoftFloat library matching the host architecture")
target_link_libraries(vortex_afu_rtl PUBLIC SystemC::systemc "${VORTEX_SOFTFLOAT_LIBRARY}")
gs_create_dymod(vortex_gpu_rtl)
target_link_libraries(vortex_gpu_rtl PRIVATE vortex_afu_rtl)
add_executable(vortex-platform-smoke smoke/platform_smoke.cc)
target_link_libraries(vortex-platform-smoke PRIVATE vortex_afu_rtl SystemC::systemc)
