# ---- low-level GPU abstraction + backends (no dependency on effects) ----------
file(GLOB AVICAP_KERNEL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.hlsl"
                                                  "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.hlsli")
set(_gpu_sources gpu/gpu_common.cpp gpu/cpu_device.cpp gpu/shader_source.cpp)
set(_gpu_libs "")
if(WIN32)
    list(APPEND _gpu_sources gpu/d3d11_device.cpp)
    set(_gpu_libs d3d11 dxgi d3dcompiler)
endif()
avicap_add_module(gpu
    SOURCES ${_gpu_sources}
    DEPS AviCap::decode ${_gpu_libs})
avicap_embed_files(avicap_gpu avc::gpu::embedded ${AVICAP_KERNEL_SOURCES})
set_source_files_properties(gpu/cpu_device.cpp PROPERTIES OBJECT_DEPENDS "${AVICAP_KERNEL_SOURCES}")

# ---- compositor (timeline -> frame), depends on effects and text -----------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compositor.cpp")
    avicap_add_module(render
        SOURCES compositor.cpp frame_provider.cpp
        DEPS AviCap::gpu AviCap::effects AviCap::text AviCap::timeline AviCap::decode)
endif()
