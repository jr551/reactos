/*
 * PROJECT:     ReactOS VirtIO GPU display miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal single-scanout 2D display driver for the QEMU
 *              virtio-gpu device (modern virtio 1.0 PCI transport).
 * COPYRIGHT:   Copyright 2026 ReactOS Portable Systems Group
 */

#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <ntdef.h>
#include <dderror.h>
#include <section_attribs.h>

/* VirtIO library headers (sdk/lib/drivers/virtio). osdep.h pulls in the
 * full DDK headers (ntddk.h/wdm.h). */
#include "osdep.h"
#include "virtio_pci.h"
#include "VirtIO.h"
#include "virtio_ring.h"

/*
 * <miniport.h> cannot be combined with ntddk.h/wdm.h in one translation
 * unit: both define the INTERFACE_TYPE/KINTERRUPT_MODE enums. <video.h>
 * relies on a few miniport-only types, which are provided here instead
 * (copied verbatim from sdk/include/ddk/miniport.h).
 */
#if !defined(_MINIPORT_)
typedef enum _EMULATOR_PORT_ACCESS_TYPE
{
    Uchar,
    Ushort,
    Ulong
} EMULATOR_PORT_ACCESS_TYPE, *PEMULATOR_PORT_ACCESS_TYPE;

typedef struct _EMULATOR_ACCESS_ENTRY
{
    ULONG BasePort;
    ULONG NumConsecutivePorts;
    EMULATOR_PORT_ACCESS_TYPE AccessType;
    UCHAR AccessMode;
    UCHAR StringSupport;
    PVOID Routine;
} EMULATOR_ACCESS_ENTRY, *PEMULATOR_ACCESS_ENTRY;

typedef VOID
(NTAPI *PBANKED_SECTION_ROUTINE)(
    IN ULONG ReadBank,
    IN ULONG WriteBank,
    IN PVOID Context);
#endif /* !defined(_MINIPORT_) */

#include <video.h>
#include <devioctl.h>

#define VIO_GPU_TAG 'pGvV' /* 'VvGp' */

/* One scanout, one resource, one control queue */
#define VIO_GPU_SCANOUT_ID      0
#define VIO_GPU_CONTROLQ_INDEX  0

/* Refresh cadence of the full-screen flush timer */
#define VIO_GPU_FLUSH_INTERVAL_MS 100

#define VIO_GPU_NO_MODE ((USHORT)-1)

/* Device responses are virtio_gpu_ctrl_hdr (24 bytes) */
#define VIO_GPU_RESPONSE_BUFFER_SIZE 128

/*
 * VirtIO GPU wire format.
 * Adapted from the BSD-licensed Linux header
 * include/uapi/linux/virtio_gpu.h (also mirrored by QEMU as
 * standard-headers/linux/virtio_gpu.h). Fields are little-endian on the
 * wire; all supported targets are x86, so plain host-endian types are used.
 */

#define VIRTIO_GPU_F_VIRGL        0
#define VIRTIO_GPU_F_EDID         1

enum virtio_gpu_ctrl_type
{
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
    VIRTIO_GPU_CMD_RESOURCE_UNREF,
    VIRTIO_GPU_CMD_SET_SCANOUT,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,

    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,

    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

/* Simple 2D formats */
enum virtio_gpu_formats
{
    VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1,
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
    VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3,
    VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4,
};

struct virtio_gpu_ctrl_hdr
{
    ULONG Type;
    ULONG Flags;
    ULONGLONG FenceId;
    ULONG CtxId;
    UCHAR RingIdx;
    UCHAR Padding[3];
};

struct virtio_gpu_rect
{
    ULONG X;
    ULONG Y;
    ULONG Width;
    ULONG Height;
};

struct virtio_gpu_resource_create_2d
{
    struct virtio_gpu_ctrl_hdr Hdr;
    ULONG ResourceId;
    ULONG Format;
    ULONG Width;
    ULONG Height;
};

struct virtio_gpu_mem_entry
{
    ULONGLONG Addr;
    ULONG Length;
    ULONG Padding;
};

struct virtio_gpu_resource_attach_backing
{
    struct virtio_gpu_ctrl_hdr Hdr;
    ULONG ResourceId;
    ULONG NumEntries;
};

struct virtio_gpu_set_scanout
{
    struct virtio_gpu_ctrl_hdr Hdr;
    struct virtio_gpu_rect R;
    ULONG ScanoutId;
    ULONG ResourceId;
};

struct virtio_gpu_resource_flush
{
    struct virtio_gpu_ctrl_hdr Hdr;
    struct virtio_gpu_rect R;
    ULONG ResourceId;
    ULONG Padding;
};

struct virtio_gpu_transfer_to_host_2d
{
    struct virtio_gpu_ctrl_hdr Hdr;
    struct virtio_gpu_rect R;
    ULONGLONG Offset;
    ULONG ResourceId;
    ULONG Padding;
};

struct virtio_gpu_resource_unref
{
    struct virtio_gpu_ctrl_hdr Hdr;
    ULONG ResourceId;
    ULONG Padding;
};

/* Device configuration (virtio_gpu_config): num_scanouts at offset 8 */
#define VIRTIO_GPU_CONFIG_NUM_SCANOUTS_OFFSET 8

typedef struct
{
    ULONG XResolution;
    ULONG YResolution;
} VIRTIO_GPU_MODE, *PVIRTIO_GPU_MODE;

typedef struct
{
    /* Mode state */
    USHORT CurrentMode;      /* VIO_GPU_NO_MODE when no mode is active */
    ULONG CurrentWidth;
    ULONG CurrentHeight;

    /* Framebuffer: guest RAM, physically contiguous, one backing entry */
    PVOID FrameBufferVa;
    PHYSICAL_ADDRESS FrameBufferPa;
    ULONG FrameBufferSize;   /* size of the maximum (4K) mode, 32 bpp */

    /* Host resource backing the current scanout */
    ULONG ResourceId;
    ULONG NextResourceId;

    /* VirtIO device */
    VirtIODevice VirtioDevice;
    struct virtqueue *ControlQueue;
    BOOLEAN DeviceReady;

    /* Control queue command/response buffers (nonpaged) */
    PVOID CommandBuffer;
    PHYSICAL_ADDRESS CommandBufferPa;
    PVOID ResponseBuffer;
    PHYSICAL_ADDRESS ResponseBufferPa;

    /* Refresh timer: the DPC queues its PASSIVE_LEVEL worker. */
    KTIMER FlushTimer;
    KDPC FlushDpc;
    WORK_QUEUE_ITEM FlushWorkItem;
    KEVENT FlushWorkIdle;
    volatile LONG FlushWorkQueued;
    volatile LONG FlushEnabled;
    BOOLEAN TimerArmed;

    /*
     * Control commands share one command/response page. They are serialized
     * with a kernel mutex because completion polling sleeps at PASSIVE_LEVEL.
     */
    KMUTEX CommandMutex;

    /* Protects the scanout state pair (ResourceId + CurrentWidth/Height)
     * shared between mode set and the refresh worker. */
    KSPIN_LOCK StateLock;

    /* Cached PCI configuration and mapped BARs for the VirtIO library */
    PCI_COMMON_CONFIG PciConfig;
    PVOID BarVa[PCI_TYPE0_ADDRESSES];
    ULONG BarLength[PCI_TYPE0_ADDRESSES];
} VIRTIO_GPU_DEVICE_EXTENSION, *PVIRTIO_GPU_DEVICE_EXTENSION;

#endif /* VIRTIO_GPU_H */
