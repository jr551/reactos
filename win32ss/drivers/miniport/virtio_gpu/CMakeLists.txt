include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/lib/drivers/virtio)

list(APPEND SOURCE
    virtio_gpu.c)

add_library(virtio_gpu MODULE ${SOURCE} virtio_gpu.rc)
set_module_type(virtio_gpu kernelmodedriver)
target_link_libraries(virtio_gpu virtio libcntpr)
add_importlibs(virtio_gpu videoprt ntoskrnl hal)
add_pch(virtio_gpu virtio_gpu.h SOURCE)
add_cd_file(TARGET virtio_gpu DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(virtio_gpu_reg.inf)
add_driver_inf(virtio_gpu virtio_gpu.inf)
