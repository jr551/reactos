/*
 * PROJECT:     ReactOS VirtIO GPU display miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal single-scanout 2D display driver for the QEMU
 *              virtio-gpu device.
 *
 *              The virtio-gpu exposes no linear framebuffer: the guest
 *              allocates a physically contiguous buffer, attaches it as the
 *              backing store of a 2D resource, and pushes dirty rectangles
 *              with TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. The host never
 *              interrupts the guest for refresh (QEMU's display-update
 *              callbacks are no-ops), so this driver runs a periodic flush
 *              timer on top of the framebuffer that framebuf.dll draws into.
 *
 *              The device is modern-only (virtio 1.0, PCI 1AF4:1050); the
 *              in-tree VirtIO library (sdk/lib/drivers/virtio) provides the
 *              transport. Scope: one scanout, 32 bpp, up to 3840x2160.
 */

#include "virtio_gpu.h"
#include <stdarg.h>


#define NDEBUG
#include <debug.h>

/*
 * The shared VirtIO transport deliberately leaves its debug sink to each
 * consumer. Unlike netkvm and viostor, this miniport has no separate debug
 * translation unit, so provide the three required definitions here.
 */
int virtioDebugLevel = 1;
int bDebugPrint = 1;

static void __cdecl
VioGpuVirtioDebugPrint(const char *Format, ...)
{
    va_list ArgList;

    va_start(ArgList, Format);
    vDbgPrintEx(DPFLTR_DEFAULT_ID, 9 | DPFLTR_MASK, Format, ArgList);
    va_end(ArgList);
}

void (__cdecl *VirtioDebugPrintProc)(const char *Format, ...) =
    VioGpuVirtioDebugPrint;

/* GLOBALS ********************************************************************/

static const VIRTIO_GPU_MODE VioGpuModes[] =
{
    { 640,  480  },
    { 800,  600  },
    { 1024, 768  },
    { 1280, 720  },
    { 1280, 1024 },
    { 1920, 1080 },
    { 2560, 1440 },
    { 3840, 2160 }, /* 4K UHD */
};

#define VIO_GPU_MODE_COUNT (sizeof(VioGpuModes) / sizeof(VioGpuModes[0]))

static WCHAR VioGpuAdapterString[] = L"VirtIO GPU";

/* Wire-format sanity checks (Linux include/uapi/linux/virtio_gpu.h) */
C_ASSERT(sizeof(struct virtio_gpu_ctrl_hdr) == 24);
C_ASSERT(sizeof(struct virtio_gpu_resource_create_2d) == 40);
C_ASSERT(sizeof(struct virtio_gpu_resource_attach_backing) == 32);
C_ASSERT(sizeof(struct virtio_gpu_mem_entry) == 16);
C_ASSERT(sizeof(struct virtio_gpu_set_scanout) == 48);
C_ASSERT(sizeof(struct virtio_gpu_resource_flush) == 48);
C_ASSERT(sizeof(struct virtio_gpu_transfer_to_host_2d) == 56);
C_ASSERT(sizeof(struct virtio_gpu_resource_unref) == 32);

/* FUNCTIONS ******************************************************************/

/*
 * VirtIO system operations
 *
 * The VirtIO library reads and writes the device registers through mapped
 * BARs (modern transport), so the register callbacks are MMIO accessors.
 */

static u8
VioGpuReadByte(ULONG_PTR Register)
{
    return READ_REGISTER_UCHAR((PUCHAR)Register);
}

static u16
VioGpuReadWord(ULONG_PTR Register)
{
    return READ_REGISTER_USHORT((PUSHORT)Register);
}

static u32
VioGpuReadDword(ULONG_PTR Register)
{
    return READ_REGISTER_ULONG((PULONG)Register);
}

static void
VioGpuWriteByte(ULONG_PTR Register, u8 Value)
{
    WRITE_REGISTER_UCHAR((PUCHAR)Register, Value);
}

static void
VioGpuWriteWord(ULONG_PTR Register, u16 Value)
{
    WRITE_REGISTER_USHORT((PUSHORT)Register, Value);
}

static void
VioGpuWriteDword(ULONG_PTR Register, u32 Value)
{
    WRITE_REGISTER_ULONG((PULONG)Register, Value);
}

/*
 * Contiguous memory for queue rings (and nothing else): no artificial cap,
 * unlike the viostor queue-block allocator. The framebuffer itself is
 * allocated directly in VioGpuInitialize.
 */
static void *
VioGpuAllocateContiguous(void *Context, size_t Size)
{
    PHYSICAL_ADDRESS HighestAcceptable;

    UNREFERENCED_PARAMETER(Context);
    HighestAcceptable.QuadPart = MAXULONG_PTR;
    return MmAllocateContiguousMemory(Size, HighestAcceptable);
}

static void
VioGpuFreeContiguous(void *Context, void *Virtual)
{
    UNREFERENCED_PARAMETER(Context);
    MmFreeContiguousMemory(Virtual);
}

static ULONGLONG
VioGpuGetPhysical(void *Context, void *Virtual)
{
    UNREFERENCED_PARAMETER(Context);
    return MmGetPhysicalAddress(Virtual).QuadPart;
}

static void *
VioGpuAllocatePool(void *Context, size_t Size)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = Context;
    PVOID Buffer;

    Buffer = VideoPortAllocatePool(Ext, VpNonPagedPool, (ULONG)Size, VIO_GPU_TAG);
    if (Buffer != NULL)
    {
        RtlZeroMemory(Buffer, Size);
    }
    return Buffer;
}

static void
VioGpuFreePool(void *Context, void *Address)
{
    VideoPortFreePool(Context, Address);
}

/*
 * The VirtIO library only needs PCI capability reads during modern probing.
 * Serve them from the PCI configuration cached in HwFindAdapter
 * (VideoPortGetBusData), as the in-tree viostor does.
 */
static int
VioGpuReadPciByte(void *Context, int Where, u8 *Value)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = Context;

    if (Where < 0 || (ULONG)Where + sizeof(*Value) > sizeof(Ext->PciConfig))
        return -1;
    *Value = *((PUCHAR)&Ext->PciConfig + Where);
    return 0;
}

static int
VioGpuReadPciWord(void *Context, int Where, u16 *Value)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = Context;

    if (Where < 0 || (ULONG)Where + sizeof(*Value) > sizeof(Ext->PciConfig))
        return -1;
    RtlCopyMemory(Value, (PUCHAR)&Ext->PciConfig + Where, sizeof(*Value));
    return 0;
}

static int
VioGpuReadPciDword(void *Context, int Where, u32 *Value)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = Context;

    if (Where < 0 || (ULONG)Where + sizeof(*Value) > sizeof(Ext->PciConfig))
        return -1;
    RtlCopyMemory(Value, (PUCHAR)&Ext->PciConfig + Where, sizeof(*Value));
    return 0;
}

static size_t
VioGpuGetResourceLength(void *Context, int Bar)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = Context;

    if (Bar < 0 || Bar >= PCI_TYPE0_ADDRESSES)
        return 0;
    return Ext->BarLength[Bar];
}

static void *
VioGpuMapAddressRange(void *Context, int Bar, size_t Offset, size_t MaxLength)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = Context;

    if (Bar < 0 || Bar >= PCI_TYPE0_ADDRESSES ||
        Ext->BarVa[Bar] == NULL ||
        Offset > Ext->BarLength[Bar] ||
        MaxLength > Ext->BarLength[Bar] - Offset)
        return NULL;
    return (PUCHAR)Ext->BarVa[Bar] + Offset;
}

static u16
VioGpuGetMsixVector(void *Context, int Queue)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Queue);
    return VIRTIO_MSI_NO_VECTOR;
}

static void
VioGpuSleep(void *Context, unsigned int Milliseconds)
{
    UNREFERENCED_PARAMETER(Context);
    KeStallExecutionProcessor(Milliseconds * 1000);
}

static const VirtIOSystemOps VioGpuSystemOps =
{
    VioGpuReadByte, VioGpuReadWord, VioGpuReadDword,
    VioGpuWriteByte, VioGpuWriteWord, VioGpuWriteDword,
    VioGpuAllocateContiguous, VioGpuFreeContiguous, VioGpuGetPhysical,
    VioGpuAllocatePool, VioGpuFreePool,
    VioGpuReadPciByte, VioGpuReadPciWord, VioGpuReadPciDword,
    VioGpuGetResourceLength, VioGpuMapAddressRange,
    VioGpuGetMsixVector, VioGpuSleep
};

/*
 * Control queue request/response
 *
 * One request in flight at a time: out descriptors (command, plus any
 * trailing data such as backing memory entries) followed by one in
 * descriptor for the response. The command and response buffers are each a
 * single physically contiguous page, so no descriptor ever straddles a
 * physical page boundary.
 */
static VP_STATUS
VioGpuSubmitCommandLocked(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                    struct VirtIOBufferDescriptor Sg[],
                    ULONG OutCount)
{
    LARGE_INTEGER Delay;
    ULONG CommandType;
    ULONG ResponseType;
    unsigned int Length;
    PVOID Opaque;
    ULONG Iterations;
    struct virtio_gpu_ctrl_hdr *Response;

    /*
     * This path shares one command and response page. The caller owns
     * CommandMutex while building the command and waiting for completion.
     * It is deliberately PASSIVE_LEVEL only: control-queue completion may
     * take up to one second and must not busy-wait while holding a spin lock.
     */
    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
    if (KeGetCurrentIrql() > APC_LEVEL)
        return ERROR_INVALID_FUNCTION;
    if (!Ext->DeviceReady || Ext->ControlQueue == NULL)
        return ERROR_DEV_NOT_EXIST;

    RtlZeroMemory(Ext->ResponseBuffer, VIO_GPU_RESPONSE_BUFFER_SIZE);

    /* Readable (in) descriptor for the response */
    Sg[OutCount].physAddr = Ext->ResponseBufferPa;
    Sg[OutCount].length = VIO_GPU_RESPONSE_BUFFER_SIZE;

    if (virtqueue_add_buf(Ext->ControlQueue, Sg, OutCount, 1,
                          Ext->ResponseBuffer, NULL, 0) != 0)
    {
        DPRINT1("VirtIO GPU: control queue full\n");
        return ERROR_INVALID_FUNCTION;
    }

    virtqueue_kick_always(Ext->ControlQueue);

    /*
     * Poll without holding a spin lock. The GPU is configured with
     * callbacks disabled, and a 1 ms delay bounds this synchronous control
     * operation to roughly one second without pinning a processor.
     */
    Opaque = NULL;
    Iterations = 0;
    Delay.QuadPart = -10 * 1000;
    while (Opaque != Ext->ResponseBuffer && Iterations++ < 1000)
    {
        Opaque = virtqueue_get_buf(Ext->ControlQueue, &Length);
        if (Opaque == NULL)
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    if (Opaque != Ext->ResponseBuffer)
    {
        /*
         * The request descriptor remains owned by the device. Do not reuse
         * the queue or its shared buffers after a timeout; reset is the only
         * operation that can safely reconstruct the queue.
         */
        Ext->DeviceReady = FALSE;
        DPRINT1("VirtIO GPU: control queue request timed out\n");
        return ERROR_DEV_NOT_EXIST;
    }

    Response = Ext->ResponseBuffer;
    ResponseType = Response->Type;
    CommandType = ((struct virtio_gpu_ctrl_hdr *)Ext->CommandBuffer)->Type;

    if (ResponseType != VIRTIO_GPU_RESP_OK_NODATA)
    {
        DPRINT1("VirtIO GPU: command 0x%lx failed with response 0x%lx\n",
                CommandType, ResponseType);
        return ERROR_DEV_NOT_EXIST;
    }
    return NO_ERROR;
}

static VP_STATUS
VioGpuCreateResource2d(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                       ULONG ResourceId,
                       ULONG Width,
                       ULONG Height)
{
    struct virtio_gpu_resource_create_2d *Cmd = Ext->CommandBuffer;
    struct VirtIOBufferDescriptor Sg[2];

    RtlZeroMemory(Cmd, sizeof(*Cmd));
    Cmd->Hdr.Type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    Cmd->ResourceId = ResourceId;
    Cmd->Format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    Cmd->Width = Width;
    Cmd->Height = Height;

    Sg[0].physAddr = Ext->CommandBufferPa;
    Sg[0].length = sizeof(*Cmd);
    return VioGpuSubmitCommandLocked(Ext, Sg, 1);
}

static VP_STATUS
VioGpuAttachBacking(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                    ULONG ResourceId,
                    PHYSICAL_ADDRESS BackingPa,
                    ULONG BackingLength)
{
    struct virtio_gpu_resource_attach_backing *Cmd = Ext->CommandBuffer;
    struct virtio_gpu_mem_entry *Entry;
    struct VirtIOBufferDescriptor Sg[3];

    RtlZeroMemory(Cmd, sizeof(*Cmd));
    Cmd->Hdr.Type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    Cmd->ResourceId = ResourceId;
    Cmd->NumEntries = 1;

    /* The memory entry array follows the command in the same request */
    Entry = (struct virtio_gpu_mem_entry *)((PUCHAR)Ext->CommandBuffer +
                                            sizeof(*Cmd));
    RtlZeroMemory(Entry, sizeof(*Entry));
    Entry->Addr = BackingPa.QuadPart;
    Entry->Length = BackingLength;

    Sg[0].physAddr = Ext->CommandBufferPa;
    Sg[0].length = sizeof(*Cmd);
    /* The entry descriptor starts right after the command within the page */
    Sg[1].physAddr = Ext->CommandBufferPa;
    Sg[1].physAddr.QuadPart += sizeof(*Cmd);
    Sg[1].length = sizeof(*Entry);
    return VioGpuSubmitCommandLocked(Ext, Sg, 2);
}

static VP_STATUS
VioGpuSetScanout(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                 ULONG ScanoutId,
                 ULONG ResourceId,
                 ULONG X,
                 ULONG Y,
                 ULONG Width,
                 ULONG Height)
{
    struct virtio_gpu_set_scanout *Cmd = Ext->CommandBuffer;
    struct VirtIOBufferDescriptor Sg[2];

    RtlZeroMemory(Cmd, sizeof(*Cmd));
    Cmd->Hdr.Type = VIRTIO_GPU_CMD_SET_SCANOUT;
    Cmd->R.X = X;
    Cmd->R.Y = Y;
    Cmd->R.Width = Width;
    Cmd->R.Height = Height;
    Cmd->ScanoutId = ScanoutId;
    Cmd->ResourceId = ResourceId;

    Sg[0].physAddr = Ext->CommandBufferPa;
    Sg[0].length = sizeof(*Cmd);
    return VioGpuSubmitCommandLocked(Ext, Sg, 1);
}

static VP_STATUS
VioGpuTransferToHost2d(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                       ULONG ResourceId,
                       ULONG X,
                       ULONG Y,
                       ULONG Width,
                       ULONG Height)
{
    struct virtio_gpu_transfer_to_host_2d *Cmd = Ext->CommandBuffer;
    struct VirtIOBufferDescriptor Sg[2];

    RtlZeroMemory(Cmd, sizeof(*Cmd));
    Cmd->Hdr.Type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    Cmd->R.X = X;
    Cmd->R.Y = Y;
    Cmd->R.Width = Width;
    Cmd->R.Height = Height;
    Cmd->Offset = 0;
    Cmd->ResourceId = ResourceId;

    Sg[0].physAddr = Ext->CommandBufferPa;
    Sg[0].length = sizeof(*Cmd);
    return VioGpuSubmitCommandLocked(Ext, Sg, 1);
}

static VP_STATUS
VioGpuFlushResource(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                    ULONG ResourceId,
                    ULONG X,
                    ULONG Y,
                    ULONG Width,
                    ULONG Height)
{
    struct virtio_gpu_resource_flush *Cmd = Ext->CommandBuffer;
    struct VirtIOBufferDescriptor Sg[2];

    RtlZeroMemory(Cmd, sizeof(*Cmd));
    Cmd->Hdr.Type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    Cmd->R.X = X;
    Cmd->R.Y = Y;
    Cmd->R.Width = Width;
    Cmd->R.Height = Height;
    Cmd->ResourceId = ResourceId;

    Sg[0].physAddr = Ext->CommandBufferPa;
    Sg[0].length = sizeof(*Cmd);
    return VioGpuSubmitCommandLocked(Ext, Sg, 1);
}

static VP_STATUS
VioGpuUnrefResource(PVIRTIO_GPU_DEVICE_EXTENSION Ext, ULONG ResourceId)
{
    struct virtio_gpu_resource_unref *Cmd = Ext->CommandBuffer;
    struct VirtIOBufferDescriptor Sg[2];

    RtlZeroMemory(Cmd, sizeof(*Cmd));
    Cmd->Hdr.Type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    Cmd->ResourceId = ResourceId;

    Sg[0].physAddr = Ext->CommandBufferPa;
    Sg[0].length = sizeof(*Cmd);
    return VioGpuSubmitCommandLocked(Ext, Sg, 1);
}

/*
 * Push the current full-screen content to the host (transfer + flush). The
 * refresh timer's DPC queues this routine, so every control command executes
 * at PASSIVE_LEVEL and can use the command mutex safely.
 */
static VOID
VioGpuFlushFullScreen(PVIRTIO_GPU_DEVICE_EXTENSION Ext)
{
    ULONG ResourceId;
    ULONG Width;
    ULONG Height;
    KIRQL Irql;

    /*
     * Hold the mutex across both the state snapshot and the command pair.
     * A mode change owns this mutex until it has destroyed the previous
     * resource, so a refresh can never submit to a stale resource ID.
     */
    if (!Ext->DeviceReady || Ext->ControlQueue == NULL ||
        !NT_SUCCESS(KeWaitForSingleObject(&Ext->CommandMutex,
                                          Executive,
                                          KernelMode,
                                          FALSE,
                                          NULL)))
    {
        return;
    }

    KeAcquireSpinLock(&Ext->StateLock, &Irql);
    ResourceId = Ext->ResourceId;
    Width = Ext->CurrentWidth;
    Height = Ext->CurrentHeight;
    KeReleaseSpinLock(&Ext->StateLock, Irql);

    if (Ext->DeviceReady &&
        Ext->ControlQueue != NULL &&
        ResourceId != 0 &&
        Width != 0 &&
        Height != 0 &&
        VioGpuTransferToHost2d(Ext, ResourceId, 0, 0, Width, Height) == NO_ERROR)
    {
        VioGpuFlushResource(Ext, ResourceId, 0, 0, Width, Height);
    }

    KeReleaseMutex(&Ext->CommandMutex, FALSE);
}

static VOID NTAPI
VioGpuFlushWorker(PVOID Parameter)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = Parameter;

    if (InterlockedCompareExchange(&Ext->FlushEnabled, 0, 0) != 0)
        VioGpuFlushFullScreen(Ext);

    InterlockedExchange(&Ext->FlushWorkQueued, 0);
    KeSetEvent(&Ext->FlushWorkIdle, IO_NO_INCREMENT, FALSE);
}

static VOID NTAPI
VioGpuFlushDpc(PKDPC Dpc,
               PVOID DeferredContext,
               PVOID SystemArgument1,
               PVOID SystemArgument2)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = DeferredContext;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (InterlockedCompareExchange(&Ext->FlushEnabled, 0, 0) == 0 ||
        InterlockedCompareExchange(&Ext->FlushWorkQueued, 1, 0) != 0)
    {
        return;
    }

    KeClearEvent(&Ext->FlushWorkIdle);
    ExQueueWorkItem(&Ext->FlushWorkItem, DelayedWorkQueue);
}

/*
 * win32k issues IOCTL_VIDEO_RESET_DEVICE ahead of every mode transition.
 * Stop and drain periodic refresh work so the following mode set owns the
 * control queue; the current resource must remain live as the fallback if
 * the new mode cannot be created.
 */
static VOID
VioGpuStopFlushWorker(PVIRTIO_GPU_DEVICE_EXTENSION Ext)
{
    InterlockedExchange(&Ext->FlushEnabled, 0);
    if (Ext->TimerArmed)
    {
        KeCancelTimer(&Ext->FlushTimer);
        Ext->TimerArmed = FALSE;
    }

    if (InterlockedCompareExchange(&Ext->FlushWorkQueued, 0, 0) != 0)
    {
        ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
        KeWaitForSingleObject(&Ext->FlushWorkIdle,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
    }
}

static ULONG
VioGpuNextResourceId(PVIRTIO_GPU_DEVICE_EXTENSION Ext)
{
    ULONG Id;

    if (Ext->NextResourceId == 0)
        Ext->NextResourceId = 1;
    Id = Ext->NextResourceId;
    Ext->NextResourceId = Id + 1;
    if (Ext->NextResourceId == 0)
        Ext->NextResourceId = 1;
    return Id;
}

CODE_SEG("PAGE")
static VOID
VioGpuGetModeInfo(PVIRTIO_GPU_MODE Mode,
                  PVIDEO_MODE_INFORMATION ModeInfo,
                  ULONG Index)
{
    VideoDebugPrint((Info, "VirtIO GPU: filling details of mode #%lu\n", Index));

    ModeInfo->Length = sizeof(*ModeInfo);
    ModeInfo->ModeIndex = Index;
    ModeInfo->VisScreenWidth = Mode->XResolution;
    ModeInfo->VisScreenHeight = Mode->YResolution;
    ModeInfo->ScreenStride = Mode->XResolution * 4;
    ModeInfo->NumberOfPlanes = 1;
    ModeInfo->BitsPerPlane = 32;
    ModeInfo->Frequency = 60;

    /* 960 DPI appears to be common (same as bochsmp) */
    ModeInfo->XMillimeter = Mode->XResolution * 254 / 960;
    ModeInfo->YMillimeter = Mode->YResolution * 254 / 960;
    ModeInfo->NumberRedBits = 8;
    ModeInfo->NumberGreenBits = 8;
    ModeInfo->NumberBlueBits = 8;
    ModeInfo->RedMask = 0xff0000;
    ModeInfo->GreenMask = 0x00ff00;
    ModeInfo->BlueMask = 0x0000ff;

    ModeInfo->AttributeFlags = VIDEO_MODE_GRAPHICS |
                               VIDEO_MODE_COLOR |
                               VIDEO_MODE_NO_OFF_SCREEN;
    ModeInfo->VideoMemoryBitmapWidth = Mode->XResolution;
    ModeInfo->VideoMemoryBitmapHeight = Mode->YResolution;
}

CODE_SEG("PAGE")
static VP_STATUS
VioGpuSetCurrentMode(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                     PVIDEO_MODE RequestedMode,
                     PSTATUS_BLOCK StatusBlock)
{
    ULONG ModeRequested = RequestedMode->RequestedMode & 0x3fffffff;
    PVIRTIO_GPU_MODE Mode;
    ULONG Width;
    ULONG Height;
    ULONG NewResourceId;
    ULONG OldResourceId;
    ULONG OldWidth;
    ULONG OldHeight;
    KIRQL Irql;
    VP_STATUS Status;
    LARGE_INTEGER DueTime;

    VideoDebugPrint((Info, "VirtIO GPU: set current mode %lu\n", ModeRequested));

    if (ModeRequested >= VIO_GPU_MODE_COUNT)
    {
        VideoDebugPrint((Error, "VirtIO GPU: set current mode - invalid parameter\n"));
        StatusBlock->Status = ERROR_INVALID_PARAMETER;
        return FALSE;
    }

    if (!Ext->DeviceReady || Ext->ControlQueue == NULL)
    {
        StatusBlock->Status = ERROR_DEV_NOT_EXIST;
        return FALSE;
    }

    Status = KeWaitForSingleObject(&Ext->CommandMutex,
                                   Executive,
                                   KernelMode,
                                   FALSE,
                                   NULL);
    if (!NT_SUCCESS(Status))
    {
        StatusBlock->Status = ERROR_DEV_NOT_EXIST;
        return FALSE;
    }
    KeAcquireSpinLock(&Ext->StateLock, &Irql);
    OldResourceId = Ext->ResourceId;
    OldWidth = Ext->CurrentWidth;
    OldHeight = Ext->CurrentHeight;
    KeReleaseSpinLock(&Ext->StateLock, Irql);


    Mode = (PVIRTIO_GPU_MODE)&VioGpuModes[ModeRequested];
    Width = Mode->XResolution;
    Height = Mode->YResolution;

    NewResourceId = VioGpuNextResourceId(Ext);

    Status = VioGpuCreateResource2d(Ext, NewResourceId, Width, Height);
    if (Status != NO_ERROR)
    {
        KeReleaseMutex(&Ext->CommandMutex, FALSE);
        StatusBlock->Status = Status;
        return FALSE;
    }

    /* Attach the whole framebuffer as a single contiguous backing entry */
    Status = VioGpuAttachBacking(Ext, NewResourceId,
                                 Ext->FrameBufferPa, Width * Height * 4);
    if (Status != NO_ERROR)
    {
        VioGpuUnrefResource(Ext, NewResourceId);
        KeReleaseMutex(&Ext->CommandMutex, FALSE);
        StatusBlock->Status = Status;
        return FALSE;
    }

    Status = VioGpuSetScanout(Ext, VIO_GPU_SCANOUT_ID, NewResourceId,
                              0, 0, Width, Height);
    if (Status != NO_ERROR)
    {
        VioGpuUnrefResource(Ext, NewResourceId);
        KeReleaseMutex(&Ext->CommandMutex, FALSE);
        StatusBlock->Status = Status;
        return FALSE;
    }

    /* A mode is not usable until its first framebuffer transfer reaches the
     * host. Roll back the uncommitted resource rather than publishing a
     * scanout whose contents cannot be refreshed. */
    Status = VioGpuTransferToHost2d(Ext, NewResourceId, 0, 0, Width, Height);
    if (Status == NO_ERROR)
        Status = VioGpuFlushResource(Ext, NewResourceId, 0, 0, Width, Height);
    if (Status != NO_ERROR)
    {
        /*
         * SET_SCANOUT already selected the uncommitted resource. Restore the
         * still-live previous scanout before dropping it; resource 0 is only
         * correct on the very first failed mode set.
         */
        VioGpuSetScanout(Ext,
                          VIO_GPU_SCANOUT_ID,
                          OldResourceId,
                          0,
                          0,
                          OldWidth,
                          OldHeight);
        VioGpuUnrefResource(Ext, NewResourceId);
        KeReleaseMutex(&Ext->CommandMutex, FALSE);
        StatusBlock->Status = Status;
        return FALSE;
    }
    /* Publish the new scanout state; the refresh DPC snapshots these fields
     * under the same lock, so it never flushes a stale resource pair. */
    KeAcquireSpinLock(&Ext->StateLock, &Irql);
    Ext->ResourceId = NewResourceId;
    Ext->CurrentMode = (USHORT)ModeRequested;
    Ext->CurrentWidth = Width;
    Ext->CurrentHeight = Height;
    KeReleaseSpinLock(&Ext->StateLock, Irql);

    /* Drop the resource of the previous mode (QEMU also disables the
     * scanout attachment as part of the resource destruction). */
    if (OldResourceId != 0 && OldResourceId != NewResourceId)
    {
        VioGpuUnrefResource(Ext, OldResourceId);
    }

    /* Start the periodic full-screen refresh. The DPC only queues the
     * PASSIVE_LEVEL worker, which owns control-queue submission. */
    InterlockedExchange(&Ext->FlushEnabled, 1);
    if (!Ext->TimerArmed)
    {
        DueTime.QuadPart = -(LONGLONG)VIO_GPU_FLUSH_INTERVAL_MS * 10000;
        KeSetTimerEx(&Ext->FlushTimer,
                     DueTime,
                     VIO_GPU_FLUSH_INTERVAL_MS,
                     &Ext->FlushDpc);
        Ext->TimerArmed = TRUE;
    }
    KeReleaseMutex(&Ext->CommandMutex, FALSE);

    StatusBlock->Status = NO_ERROR;
    VideoDebugPrint((Info, "VirtIO GPU: mode %lux%lu set\n", Width, Height));
    return TRUE;
}

CODE_SEG("PAGE")
static BOOLEAN
VioGpuQueryCurrentMode(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                       PVIDEO_MODE_INFORMATION VideoModeInfo,
                       PSTATUS_BLOCK StatusBlock)
{
    PVIRTIO_GPU_MODE Mode;

    if (Ext->CurrentMode >= VIO_GPU_MODE_COUNT)
    {
        StatusBlock->Status = ERROR_INVALID_PARAMETER;
        return FALSE;
    }

    Mode = (PVIRTIO_GPU_MODE)&VioGpuModes[Ext->CurrentMode];
    VideoPortZeroMemory(VideoModeInfo, sizeof(*VideoModeInfo));
    VioGpuGetModeInfo(Mode, VideoModeInfo, Ext->CurrentMode);

    StatusBlock->Information = sizeof(*VideoModeInfo);
    StatusBlock->Status = NO_ERROR;
    return TRUE;
}

CODE_SEG("PAGE")
static BOOLEAN
VioGpuQueryNumAvailableModes(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                             PVIDEO_NUM_MODES AvailableModes,
                             PSTATUS_BLOCK StatusBlock)
{
    AvailableModes->NumModes = VIO_GPU_MODE_COUNT;
    AvailableModes->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);

    StatusBlock->Information = sizeof(*AvailableModes);
    StatusBlock->Status = NO_ERROR;
    return TRUE;
}

CODE_SEG("PAGE")
static BOOLEAN
VioGpuQueryAvailableModes(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                          PVIDEO_MODE_INFORMATION ReturnedModes,
                          PSTATUS_BLOCK StatusBlock)
{
    ULONG Count;
    PVIDEO_MODE_INFORMATION ModeInfo;

    for (Count = 0, ModeInfo = ReturnedModes;
         Count < VIO_GPU_MODE_COUNT;
         Count++, ModeInfo++)
    {
        VideoPortZeroMemory(ModeInfo, sizeof(*ModeInfo));
        VioGpuGetModeInfo((PVIRTIO_GPU_MODE)&VioGpuModes[Count],
                          ModeInfo,
                          Count);
    }

    StatusBlock->Information = sizeof(VIDEO_MODE_INFORMATION) * VIO_GPU_MODE_COUNT;
    StatusBlock->Status = NO_ERROR;
    return TRUE;
}

CODE_SEG("PAGE")
static BOOLEAN
VioGpuMapVideoMemory(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                     PVIDEO_MEMORY RequestedAddress,
                     PVIDEO_MEMORY_INFORMATION MapInformation,
                     PSTATUS_BLOCK StatusBlock)
{
    /* The framebuffer is guest RAM allocated and mapped by this driver;
     * framebuf.dll (a kernel-mode DLL) draws into the returned VA directly,
     * so no VideoPortMapMemory round-trip through the PCI bus is needed. */
    UNREFERENCED_PARAMETER(RequestedAddress);

    if (Ext->FrameBufferVa == NULL)
    {
        StatusBlock->Status = ERROR_DEV_NOT_EXIST;
        return FALSE;
    }

    MapInformation->VideoRamBase = Ext->FrameBufferVa;
    MapInformation->VideoRamLength = Ext->FrameBufferSize;
    MapInformation->FrameBufferBase = Ext->FrameBufferVa;
    MapInformation->FrameBufferLength = Ext->FrameBufferSize;
    StatusBlock->Information = sizeof(*MapInformation);
    StatusBlock->Status = NO_ERROR;
    return TRUE;
}

CODE_SEG("PAGE")
static BOOLEAN
VioGpuUnmapVideoMemory(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                       PVIDEO_MEMORY VideoMemory,
                       PSTATUS_BLOCK StatusBlock)
{
    /* The mapping is owned by the miniport for the driver's lifetime. */
    UNREFERENCED_PARAMETER(Ext);
    UNREFERENCED_PARAMETER(VideoMemory);
    StatusBlock->Status = NO_ERROR;
    return TRUE;
}

CODE_SEG("PAGE")
static BOOLEAN
VioGpuResetDevice(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                  PSTATUS_BLOCK StatusBlock)
{
    VideoDebugPrint((Info, "VirtIO GPU: reset device\n"));

    /*
     * Keep ResourceId and the current scanout intact. VioGpuSetCurrentMode
     * creates, attaches, scans out, and flushes its replacement before it
     * unrefs the old resource, so this is make-before-break even when the
     * caller falls back after a failed mode change.
     */
    VioGpuStopFlushWorker(Ext);
    StatusBlock->Status = NO_ERROR;
    return TRUE;
}

CODE_SEG("PAGE")
static BOOLEAN
VioGpuGetChildState(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
                    PULONG pChildState,
                    PSTATUS_BLOCK StatusBlock)
{
    *pChildState = VIDEO_CHILD_ACTIVE;

    StatusBlock->Information = sizeof(*pChildState);
    StatusBlock->Status = NO_ERROR;
    return TRUE;
}

CODE_SEG("PAGE")
static VP_STATUS NTAPI
VioGpuGetVideoChildDescriptor(PVOID HwDeviceExtension,
                              PVIDEO_CHILD_ENUM_INFO ChildEnumInfo,
                              PVIDEO_CHILD_TYPE VideoChildType,
                              PUCHAR pChildDescriptor,
                              PULONG UId,
                              PULONG pUnused)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = HwDeviceExtension;

    VideoDebugPrint((Info, "VirtIO GPU: get child descriptor %lu\n",
                     ChildEnumInfo->ChildIndex));

    if (ChildEnumInfo->Size < sizeof(*VideoChildType))
        return VIDEO_ENUM_NO_MORE_DEVICES;

    if (ChildEnumInfo->ChildIndex == 0)
        return VIDEO_ENUM_INVALID_DEVICE;

    *pUnused = 0;
    if (ChildEnumInfo->ChildIndex == DISPLAY_ADAPTER_HW_ID)
    {
        *VideoChildType = VideoChip;
        return VIDEO_ENUM_MORE_DEVICES;
    }

    if (ChildEnumInfo->ChildIndex != 1)
        return VIDEO_ENUM_NO_MORE_DEVICES;

    *UId = 0;
    *VideoChildType = Monitor;
    /* No EDID support: report a monitor without a descriptor */
    if (pChildDescriptor != NULL)
    {
        RtlZeroMemory(pChildDescriptor, ChildEnumInfo->ChildDescriptorSize);
    }

    return VIDEO_ENUM_MORE_DEVICES;
}

CODE_SEG("PAGE")
BOOLEAN NTAPI
VioGpuStartIO(PVOID HwDeviceExtension,
              PVIDEO_REQUEST_PACKET RequestPacket)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = HwDeviceExtension;

    VideoDebugPrint((Info, "VirtIO GPU: StartIO 0x%lx\n",
                     RequestPacket->IoControlCode));
    RequestPacket->StatusBlock->Status = ERROR_INVALID_FUNCTION;

    switch (RequestPacket->IoControlCode)
    {
        case IOCTL_VIDEO_MAP_VIDEO_MEMORY:
        {
            if (RequestPacket->InputBufferLength < sizeof(VIDEO_MEMORY))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return FALSE;
            }
            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_MEMORY_INFORMATION))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return FALSE;
            }
            return VioGpuMapVideoMemory(Ext,
                                        (PVIDEO_MEMORY)RequestPacket->InputBuffer,
                                        (PVIDEO_MEMORY_INFORMATION)RequestPacket->OutputBuffer,
                                        RequestPacket->StatusBlock);
        }

        case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        {
            if (RequestPacket->InputBufferLength < sizeof(VIDEO_MEMORY))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return FALSE;
            }
            return VioGpuUnmapVideoMemory(Ext,
                                          (PVIDEO_MEMORY)RequestPacket->InputBuffer,
                                          RequestPacket->StatusBlock);
        }

        case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES:
        {
            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_NUM_MODES))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return FALSE;
            }
            return VioGpuQueryNumAvailableModes(Ext,
                                                (PVIDEO_NUM_MODES)RequestPacket->OutputBuffer,
                                                RequestPacket->StatusBlock);
        }

        case IOCTL_VIDEO_QUERY_AVAIL_MODES:
        {
            if (RequestPacket->OutputBufferLength <
                VIO_GPU_MODE_COUNT * sizeof(VIDEO_MODE_INFORMATION))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return FALSE;
            }
            return VioGpuQueryAvailableModes(Ext,
                                             (PVIDEO_MODE_INFORMATION)RequestPacket->OutputBuffer,
                                             RequestPacket->StatusBlock);
        }

        case IOCTL_VIDEO_SET_CURRENT_MODE:
        {
            if (RequestPacket->InputBufferLength < sizeof(VIDEO_MODE))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return FALSE;
            }
            return VioGpuSetCurrentMode(Ext,
                                        (PVIDEO_MODE)RequestPacket->InputBuffer,
                                        RequestPacket->StatusBlock);
        }

        case IOCTL_VIDEO_QUERY_CURRENT_MODE:
        {
            if (RequestPacket->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return FALSE;
            }
            return VioGpuQueryCurrentMode(Ext,
                                          (PVIDEO_MODE_INFORMATION)RequestPacket->OutputBuffer,
                                          RequestPacket->StatusBlock);
        }

        case IOCTL_VIDEO_RESET_DEVICE:
        {
            return VioGpuResetDevice(Ext, RequestPacket->StatusBlock);
        }

        case IOCTL_VIDEO_GET_CHILD_STATE:
        {
            if (RequestPacket->OutputBufferLength < sizeof(ULONG))
            {
                RequestPacket->StatusBlock->Status = ERROR_INSUFFICIENT_BUFFER;
                return FALSE;
            }
            return VioGpuGetChildState(Ext,
                                       (PULONG)RequestPacket->OutputBuffer,
                                       RequestPacket->StatusBlock);
        }

        default:
        {
            VideoDebugPrint((Warn, "VirtIO GPU: unknown IOCTL 0x%lx\n",
                             RequestPacket->IoControlCode));
            break;
        }
    }

    return FALSE;
}

CODE_SEG("PAGE")
static VOID
VioGpuMapBars(PVIRTIO_GPU_DEVICE_EXTENSION Ext,
              PVIDEO_ACCESS_RANGE AccessRanges,
              ULONG RangeCount)
{
    ULONG Index;
    ULONG Range;

    for (Index = 0; Index < PCI_TYPE0_ADDRESSES; Index++)
    {
        ULONG Value = Ext->PciConfig.u.type0.BaseAddresses[Index];
        PHYSICAL_ADDRESS Phys;
        UCHAR InIoSpace;
        /* A 64-bit BAR consumes two config slots, but it is still BAR<Index>
         * to everyone else. Keep its logical index before skipping the high
         * dword, or the VirtIO capability BAR can be recorded one slot high. */
        ULONG BarIndex = Index;

        if (Value == 0)
            continue;

        Phys.QuadPart = 0;
        InIoSpace = FALSE;
        if (Value & 1)
        {
            /* I/O space BAR */
            Phys.LowPart = Value & ~3;
            InIoSpace = TRUE;
        }
        else if ((Value & 0x6) == 0x4)
        {
            /* 64-bit memory BAR (consumes two slots) */
            if (Index + 1 >= PCI_TYPE0_ADDRESSES)
                continue;
            Phys.LowPart = Value & ~0xF;
            Phys.HighPart = Ext->PciConfig.u.type0.BaseAddresses[Index + 1];
            Index++;
        }
        else
        {
            /* 32-bit memory BAR */
            Phys.LowPart = Value & ~0xF;
        }

        /* Match the claimed range to learn the BAR size */
        for (Range = 0; Range < RangeCount; Range++)
        {
            if (AccessRanges[Range].RangeStart.QuadPart == Phys.QuadPart)
            {
                Ext->BarLength[BarIndex] = AccessRanges[Range].RangeLength;
                break;
            }
        }

        if (Ext->BarLength[BarIndex] == 0)
            continue;

        Ext->BarVa[BarIndex] = VideoPortGetDeviceBase(Ext,
                                                   Phys,
                                                   Ext->BarLength[BarIndex],
                                                   InIoSpace
                                                       ? VIDEO_MEMORY_SPACE_IO
                                                       : VIDEO_MEMORY_SPACE_MEMORY);
        VideoDebugPrint((Info, "VirtIO GPU: BAR%lu at 0x%llx len 0x%lx mapped to %p\n",
                         BarIndex, Phys.QuadPart, Ext->BarLength[BarIndex],
                         Ext->BarVa[BarIndex]));
    }
}

CODE_SEG("PAGE")
VP_STATUS NTAPI
VioGpuFindAdapter(PVOID HwDeviceExtension,
                  PVOID HwContext,
                  PWSTR ArgumentString,
                  PVIDEO_PORT_CONFIG_INFO ConfigInfo,
                  PUCHAR Again)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = HwDeviceExtension;
    VIDEO_ACCESS_RANGE AccessRanges[8];
    ULONG RangeCount;
    ULONG ReturnedLength;
    ULONG MemorySize;

    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(ArgumentString);
    UNREFERENCED_PARAMETER(Again);

    VideoDebugPrint((Info, "VirtIO GPU: searching for adapter\n"));

    VideoPortZeroMemory(Ext, sizeof(*Ext));
    VideoPortZeroMemory(AccessRanges, sizeof(AccessRanges));
    Ext->CurrentMode = VIO_GPU_NO_MODE;

    if (ConfigInfo->Length < sizeof(VIDEO_PORT_CONFIG_INFO))
    {
        VideoDebugPrint((Error, "VirtIO GPU: invalid configuration info\n"));
        return ERROR_INVALID_PARAMETER;
    }

    /* Cache the PCI configuration space for the VirtIO library */
    ReturnedLength = VideoPortGetBusData(Ext,
                                         PCIConfiguration,
                                         0,
                                         &Ext->PciConfig,
                                         0,
                                         sizeof(Ext->PciConfig));
    /* The header and BARs are all the VirtIO library needs. */
    if (ReturnedLength < FIELD_OFFSET(PCI_COMMON_CONFIG, u.type0.BaseAddresses))
    {
        VideoDebugPrint((Error, "VirtIO GPU: short PCI configuration read (%lu bytes)\n",
                         ReturnedLength));
        return ERROR_DEV_NOT_EXIST;
    }

    /* Only bind to the QEMU virtio-gpu (modern 0x1050, transitional 0x1010) */
    if (Ext->PciConfig.VendorID != 0x1AF4 ||
        (Ext->PciConfig.DeviceID != 0x1050 && Ext->PciConfig.DeviceID != 0x1010))
    {
        VideoDebugPrint((Error, "VirtIO GPU: unexpected PCI device %04x:%04x\n",
                         Ext->PciConfig.VendorID, Ext->PciConfig.DeviceID));
        return ERROR_DEV_NOT_EXIST;
    }

    /* Claim the device resources */
    if (VideoPortGetAccessRanges(Ext,
                                 0,
                                 NULL,
                                 ARRAYSIZE(AccessRanges),
                                 AccessRanges,
                                 NULL,
                                 NULL,
                                 NULL) != NO_ERROR)
    {
        VideoDebugPrint((Error, "VirtIO GPU: failed to get access ranges\n"));
        return ERROR_DEV_NOT_EXIST;
    }

    RangeCount = ARRAYSIZE(AccessRanges);
    while (RangeCount > 0 && AccessRanges[RangeCount - 1].RangeLength == 0)
        RangeCount--;

    VioGpuMapBars(Ext, AccessRanges, RangeCount);

    /* Store information in registry */
    VideoPortSetRegistryParameters(Ext,
                                   L"HardwareInformation.ChipType",
                                   VioGpuAdapterString,
                                   sizeof(VioGpuAdapterString));
    VideoPortSetRegistryParameters(Ext,
                                   L"HardwareInformation.DacType",
                                   VioGpuAdapterString,
                                   sizeof(VioGpuAdapterString));
    VideoPortSetRegistryParameters(Ext,
                                   L"HardwareInformation.AdapterString",
                                   VioGpuAdapterString,
                                   sizeof(VioGpuAdapterString));
    VideoPortSetRegistryParameters(Ext,
                                   L"HardwareInformation.BiosString",
                                   VioGpuAdapterString,
                                   sizeof(VioGpuAdapterString));
    /* Report the framebuffer the driver will allocate for the max (4K) mode */
    MemorySize = VioGpuModes[VIO_GPU_MODE_COUNT - 1].XResolution *
                 VioGpuModes[VIO_GPU_MODE_COUNT - 1].YResolution * 4;
    VideoPortSetRegistryParameters(Ext,
                                   L"HardwareInformation.MemorySize",
                                   &MemorySize,
                                   sizeof(MemorySize));

    ConfigInfo->NumEmulatorAccessEntries = 0;
    ConfigInfo->EmulatorAccessEntries = NULL;
    ConfigInfo->EmulatorAccessEntriesContext = 0;
    ConfigInfo->HardwareStateSize = 0;
    ConfigInfo->VdmPhysicalVideoMemoryAddress.QuadPart = 0;
    ConfigInfo->VdmPhysicalVideoMemoryLength = 0;

    return NO_ERROR;
}

CODE_SEG("PAGE")
BOOLEAN NTAPI
VioGpuInitialize(PVOID HwDeviceExtension)
{
    PVIRTIO_GPU_DEVICE_EXTENSION Ext = HwDeviceExtension;
    PHYSICAL_ADDRESS HighestAcceptable;
    ULONGLONG Features;
    NTSTATUS Status;
    ULONG NumScanouts;
    PVIRTIO_GPU_MODE MaxMode;
    ULONG Index;

    VideoDebugPrint((Info, "VirtIO GPU: initialize\n"));

    KeInitializeMutex(&Ext->CommandMutex, 0);
    KeInitializeSpinLock(&Ext->StateLock);
    KeInitializeTimer(&Ext->FlushTimer);
    KeInitializeDpc(&Ext->FlushDpc, VioGpuFlushDpc, Ext);
    ExInitializeWorkItem(&Ext->FlushWorkItem, VioGpuFlushWorker, Ext);
    KeInitializeEvent(&Ext->FlushWorkIdle, NotificationEvent, TRUE);
    /* Framebuffer for the maximum (4K) mode, 32 bpp, physically contiguous:
     * 3840 * 2160 * 4 = 33,177,600 bytes. One allocation serves every mode;
     * smaller modes attach a slice of it as the resource backing. */
    MaxMode = (PVIRTIO_GPU_MODE)&VioGpuModes[VIO_GPU_MODE_COUNT - 1];
    Ext->FrameBufferSize = MaxMode->XResolution * MaxMode->YResolution * 4;
    HighestAcceptable.QuadPart = MAXULONG_PTR;
    Ext->FrameBufferVa = VideoPortAllocateContiguousMemory(Ext,
                                                           Ext->FrameBufferSize,
                                                           HighestAcceptable);
    if (Ext->FrameBufferVa == NULL)
    {
        VideoDebugPrint((Error, "VirtIO GPU: failed to allocate %lu bytes "
                         "of contiguous memory for the framebuffer\n",
                         Ext->FrameBufferSize));
        return FALSE;
    }
    VideoPortZeroMemory(Ext->FrameBufferVa, Ext->FrameBufferSize);
    Ext->FrameBufferPa = MmGetPhysicalAddress(Ext->FrameBufferVa);
    VideoDebugPrint((Info, "VirtIO GPU: framebuffer %p phys 0x%llx size 0x%lx\n",
                     Ext->FrameBufferVa, Ext->FrameBufferPa.QuadPart,
                     Ext->FrameBufferSize));

    /* Command and response buffers: one page-aligned page each, so no
     * control queue descriptor ever straddles a physical page boundary
     * (the host reads each descriptor's range as one contiguous range). */
    Ext->CommandBuffer = MmAllocateNonCachedMemory(PAGE_SIZE);
    Ext->ResponseBuffer = MmAllocateNonCachedMemory(PAGE_SIZE);
    if (Ext->CommandBuffer == NULL || Ext->ResponseBuffer == NULL)
    {
        goto Failure;
    }
    Ext->CommandBufferPa = MmGetPhysicalAddress(Ext->CommandBuffer);
    Ext->ResponseBufferPa = MmGetPhysicalAddress(Ext->ResponseBuffer);

    /* Bring up the VirtIO device (modern transport; QEMU's virtio-gpu has no
     * legacy interface) */
    Status = virtio_device_initialize(&Ext->VirtioDevice,
                                      &VioGpuSystemOps,
                                      Ext,
                                      FALSE);
    if (!NT_SUCCESS(Status))
    {
        VideoDebugPrint((Error, "VirtIO GPU: virtio_device_initialize failed 0x%lx\n",
                         Status));
        goto Failure;
    }

    Features = virtio_get_features(&Ext->VirtioDevice);
    /* 2D only: no virgl, no EDID, no blobs. Keep VIRTIO_F_VERSION_1 (the
     * modern transport requires it); no ring features (direct descriptors). */
    Features &= (1ULL << VIRTIO_F_VERSION_1);
    Status = virtio_set_features(&Ext->VirtioDevice, Features);
    if (!NT_SUCCESS(Status))
    {
        VideoDebugPrint((Error, "VirtIO GPU: virtio_set_features failed 0x%lx\n",
                         Status));
        goto Failure;
    }

    Status = virtio_find_queue(&Ext->VirtioDevice, VIO_GPU_CONTROLQ_INDEX,
                               &Ext->ControlQueue);
    if (!NT_SUCCESS(Status))
    {
        VideoDebugPrint((Error, "VirtIO GPU: control queue not found 0x%lx\n",
                         Status));
        goto Failure;
    }

    /* Polling mode: keep the device from raising INTx on used buffers */
    virtqueue_disable_cb(Ext->ControlQueue);

    NumScanouts = 0;
    virtio_get_config(&Ext->VirtioDevice,
                      VIRTIO_GPU_CONFIG_NUM_SCANOUTS_OFFSET,
                      &NumScanouts,
                      sizeof(NumScanouts));
    if (NumScanouts < 1)
    {
        VideoDebugPrint((Error, "VirtIO GPU: device reports %lu scanouts\n",
                         NumScanouts));
        goto Failure;
    }

    virtio_device_ready(&Ext->VirtioDevice);
    Ext->DeviceReady = TRUE;
    return TRUE;

Failure:
    if (Ext->ControlQueue != NULL)
    {
        virtio_delete_queue(Ext->ControlQueue);
        Ext->ControlQueue = NULL;
    }
    if (Ext->VirtioDevice.device != NULL)
    {
        virtio_device_reset(&Ext->VirtioDevice);
    }
    virtio_device_shutdown(&Ext->VirtioDevice);
    if (Ext->ResponseBuffer != NULL)
    {
        MmFreeNonCachedMemory(Ext->ResponseBuffer, PAGE_SIZE);
        Ext->ResponseBuffer = NULL;
    }
    if (Ext->CommandBuffer != NULL)
    {
        MmFreeNonCachedMemory(Ext->CommandBuffer, PAGE_SIZE);
        Ext->CommandBuffer = NULL;
    }
    if (Ext->FrameBufferVa != NULL)
    {
        MmFreeContiguousMemory(Ext->FrameBufferVa);
        Ext->FrameBufferVa = NULL;
        Ext->FrameBufferSize = 0;
    }
    return FALSE;
}

CODE_SEG("PAGE")
VP_STATUS NTAPI
VioGpuSetPowerState(PVOID HwDeviceExtension,
                    ULONG HwId,
                    PVIDEO_POWER_MANAGEMENT VideoPowerControl)
{
    return NO_ERROR;
}

CODE_SEG("PAGE")
VP_STATUS NTAPI
VioGpuGetPowerState(PVOID HwDeviceExtension,
                    ULONG HwId,
                    PVIDEO_POWER_MANAGEMENT VideoPowerControl)
{
    return ERROR_DEVICE_REINITIALIZATION_NEEDED;
}

ULONG NTAPI
DriverEntry(PVOID Context1, PVOID Context2)
{
    VIDEO_HW_INITIALIZATION_DATA VideoInitData;

    VideoDebugPrint((Info, "VirtIO GPU: DriverEntry\n"));
    VideoPortZeroMemory(&VideoInitData, sizeof(VideoInitData));
    VideoInitData.HwInitDataSize = sizeof(VideoInitData);
    VideoInitData.HwFindAdapter = VioGpuFindAdapter;
    VideoInitData.HwInitialize = VioGpuInitialize;
    VideoInitData.HwStartIO = VioGpuStartIO;
    VideoInitData.HwDeviceExtensionSize = sizeof(VIRTIO_GPU_DEVICE_EXTENSION);
    VideoInitData.HwSetPowerState = VioGpuSetPowerState;
    VideoInitData.HwGetPowerState = VioGpuGetPowerState;
    VideoInitData.HwGetVideoChildDescriptor = VioGpuGetVideoChildDescriptor;

    return VideoPortInitialize(Context1, Context2, &VideoInitData, NULL);
}
