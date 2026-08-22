/*
 * PROJECT:     ReactOS Storport Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Storport driver main file
 * COPYRIGHT:   Copyright 2017 Eric Kohl (eric.kohl@reactos.org)
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

#define NDEBUG
#include <debug.h>


/* GLOBALS ********************************************************************/

ULONG PortNumber = 0;


/* FUNCTIONS ******************************************************************/

static
NTSTATUS
PortAddDriverInitData(
    PDRIVER_OBJECT_EXTENSION DriverExtension,
    PHW_INITIALIZATION_DATA HwInitializationData)
{
    PDRIVER_INIT_DATA InitData;

    DPRINT1("PortAddDriverInitData()\n");

    InitData = ExAllocatePoolWithTag(NonPagedPool,
                                     sizeof(DRIVER_INIT_DATA),
                                     TAG_INIT_DATA);
    if (InitData == NULL)
        return STATUS_NO_MEMORY;

    RtlCopyMemory(&InitData->HwInitData,
                  HwInitializationData,
                  sizeof(HW_INITIALIZATION_DATA));

    InsertHeadList(&DriverExtension->InitDataListHead,
                   &InitData->Entry);

    return STATUS_SUCCESS;
}


static
VOID
PortDeleteDriverInitData(
    PDRIVER_OBJECT_EXTENSION DriverExtension)
{
    PDRIVER_INIT_DATA InitData;
    PLIST_ENTRY ListEntry;

    DPRINT1("PortDeleteDriverInitData()\n");

    ListEntry = DriverExtension->InitDataListHead.Flink;
    while (ListEntry != &DriverExtension->InitDataListHead)
    {
        InitData = CONTAINING_RECORD(ListEntry,
                                     DRIVER_INIT_DATA,
                                     Entry);

        RemoveEntryList(&InitData->Entry);

        ExFreePoolWithTag(InitData,
                          TAG_INIT_DATA);

        ListEntry = DriverExtension->InitDataListHead.Flink;
    }
}


PHW_INITIALIZATION_DATA
PortGetDriverInitData(
    PDRIVER_OBJECT_EXTENSION DriverExtension,
    INTERFACE_TYPE InterfaceType)
{
    PDRIVER_INIT_DATA InitData;
    PLIST_ENTRY ListEntry;

    DPRINT1("PortGetDriverInitData()\n");

    ListEntry = DriverExtension->InitDataListHead.Flink;
    while (ListEntry != &DriverExtension->InitDataListHead)
    {
        InitData = CONTAINING_RECORD(ListEntry,
                                     DRIVER_INIT_DATA,
                                     Entry);
        if (InitData->HwInitData.AdapterInterfaceType == InterfaceType)
            return &InitData->HwInitData;

        ListEntry = ListEntry->Flink;
    }

    return NULL;
}


static
VOID
PortAcquireSpinLock(
    PFDO_DEVICE_EXTENSION DeviceExtension,
    STOR_SPINLOCK SpinLock,
    PVOID LockContext,
    PSTOR_LOCK_HANDLE LockHandle)
{
    DPRINT1("PortAcquireSpinLock(%p %lu %p %p)\n",
            DeviceExtension, SpinLock, LockContext, LockHandle);

    LockHandle->Lock = SpinLock;

    switch (SpinLock)
    {
        case DpcLock: /* 1, */
            DPRINT1("DpcLock\n");
            break;

        case StartIoLock: /* 2 */
            DPRINT1("StartIoLock\n");
            break;

        case InterruptLock: /* 3 */
            DPRINT1("InterruptLock\n");
            if (DeviceExtension->Interrupt == NULL)
                LockHandle->Context.OldIrql = 0;
            else
                LockHandle->Context.OldIrql = KeAcquireInterruptSpinLock(DeviceExtension->Interrupt);
            break;
    }
}


static
VOID
PortReleaseSpinLock(
    PFDO_DEVICE_EXTENSION DeviceExtension,
    PSTOR_LOCK_HANDLE LockHandle)
{
    DPRINT1("PortReleaseSpinLock(%p %p)\n",
            DeviceExtension, LockHandle);

    switch (LockHandle->Lock)
    {
        case DpcLock: /* 1, */
            DPRINT1("DpcLock\n");
            break;

        case StartIoLock: /* 2 */
            DPRINT1("StartIoLock\n");
            break;

        case InterruptLock: /* 3 */
            DPRINT1("InterruptLock\n");
            if (DeviceExtension->Interrupt != NULL)
                KeReleaseInterruptSpinLock(DeviceExtension->Interrupt,
                                           LockHandle->Context.OldIrql);
            break;
    }
}


static
NTSTATUS
NTAPI
PortAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDRIVER_OBJECT_EXTENSION DriverObjectExtension;
    PFDO_DEVICE_EXTENSION DeviceExtension = NULL;
    WCHAR NameBuffer[80];
    UNICODE_STRING DeviceName;
    PDEVICE_OBJECT Fdo = NULL;
    KLOCK_QUEUE_HANDLE LockHandle;
    NTSTATUS Status;

    DPRINT1("PortAddDevice(%p %p)\n",
            DriverObject, PhysicalDeviceObject);

    ASSERT(DriverObject);
    ASSERT(PhysicalDeviceObject);

    _swprintf(NameBuffer,
              L"\\Device\\RaidPort%lu",
              PortNumber);
    RtlInitUnicodeString(&DeviceName, NameBuffer);
    PortNumber++;

    DPRINT1("Creating device: %wZ\n", &DeviceName);

    /* Create the port device */
    Status = IoCreateDevice(DriverObject,
                            sizeof(FDO_DEVICE_EXTENSION),
                            &DeviceName,
                            FILE_DEVICE_CONTROLLER,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &Fdo);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IoCreateDevice() failed (Status 0x%08lx)\n", Status);
        return Status;
    }

    DPRINT1("Created device: %wZ (%p)\n", &DeviceName, Fdo);

    /* Initialize the device */
    Fdo->Flags |= DO_DIRECT_IO;
    Fdo->Flags |= DO_POWER_PAGABLE;

    /* Initialize the device extension */
    DeviceExtension = (PFDO_DEVICE_EXTENSION)Fdo->DeviceExtension;
    RtlZeroMemory(DeviceExtension, sizeof(FDO_DEVICE_EXTENSION));

    DeviceExtension->ExtensionType = FdoExtension;

    DeviceExtension->Device = Fdo;
    DeviceExtension->PhysicalDevice = PhysicalDeviceObject;

    DeviceExtension->PnpState = dsStopped;

    KeInitializeSpinLock(&DeviceExtension->PdoListLock);
    InitializeListHead(&DeviceExtension->PdoListHead);
    ExInitializeSListHead(&DeviceExtension->CompletionList);
    KeInitializeDpc(&DeviceExtension->CompletionDpc,
                    PortCompletionDpc,
                    DeviceExtension);

    /* Attach the FDO to the device stack */
    Status = IoAttachDeviceToDeviceStackSafe(Fdo,
                                             PhysicalDeviceObject,
                                             &DeviceExtension->LowerDevice);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IoAttachDeviceToDeviceStackSafe() failed (Status 0x%08lx)\n", Status);
        IoDeleteDevice(Fdo);
        return Status;
    }

    /* Insert the FDO to the drivers FDO list */
    DriverObjectExtension = IoGetDriverObjectExtension(DriverObject,
                                                       (PVOID)DriverEntry);
    ASSERT(DriverObjectExtension->ExtensionType == DriverExtension);

    DeviceExtension->DriverExtension = DriverObjectExtension;

    KeAcquireInStackQueuedSpinLock(&DriverObjectExtension->AdapterListLock,
                                   &LockHandle);

    InsertHeadList(&DriverObjectExtension->AdapterListHead,
                   &DeviceExtension->AdapterListEntry);
    DriverObjectExtension->AdapterCount++;

    KeReleaseInStackQueuedSpinLock(&LockHandle);

    /* The device has been initialized */
    Fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    DPRINT1("PortAddDevice() done (Status 0x%08lx)\n", Status);

    return Status;
}


static
VOID
NTAPI
PortUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PDRIVER_OBJECT_EXTENSION DriverExtension;

    DPRINT1("PortUnload(%p)\n",
            DriverObject);

    DriverExtension = IoGetDriverObjectExtension(DriverObject,
                                                 (PVOID)DriverEntry);
    if (DriverExtension != NULL)
    {
        PortDeleteDriverInitData(DriverExtension);
    }
}


static
NTSTATUS
NTAPI
PortDispatchCreate(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    DPRINT1("PortDispatchCreate(%p %p)\n",
            DeviceObject, Irp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = FILE_OPENED;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NTAPI
PortDispatchClose(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    DPRINT1("PortDispatchClose(%p %p)\n",
            DeviceObject, Irp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NTAPI
PortDispatchDeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    DPRINT1("PortDispatchDeviceControl(%p %p)\n",
            DeviceObject, Irp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NTAPI
PortDispatchScsi(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PFDO_DEVICE_EXTENSION DeviceExtension;

    DPRINT1("PortDispatchScsi(%p %p)\n",
            DeviceObject, Irp);

    DeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    DPRINT1("ExtensionType: %u\n", DeviceExtension->ExtensionType);

    switch (DeviceExtension->ExtensionType)
    {
        case FdoExtension:
            return PortFdoScsi(DeviceObject,
                               Irp);

        case PdoExtension:
            return PortPdoScsi(DeviceObject,
                               Irp);

        default:
            Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}


static
NTSTATUS
NTAPI
PortDispatchSystemControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    DPRINT1("PortDispatchSystemControl(%p %p)\n",
            DeviceObject, Irp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NTAPI
PortDispatchPnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    PFDO_DEVICE_EXTENSION DeviceExtension;

    DPRINT1("PortDispatchPnp(%p %p)\n",
            DeviceObject, Irp);

    DeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    DPRINT1("ExtensionType: %u\n", DeviceExtension->ExtensionType);

    switch (DeviceExtension->ExtensionType)
    {
        case FdoExtension:
            return PortFdoPnp(DeviceObject,
                              Irp);

        case PdoExtension:
            return PortPdoPnp(DeviceObject,
                              Irp);

        default:
            Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_UNSUCCESSFUL;
    }
}


static
NTSTATUS
NTAPI
PortDispatchPower(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp)
{
    DPRINT1("PortDispatchPower(%p %p)\n",
            DeviceObject, Irp);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    DPRINT1("DriverEntry(%p %p)\n", DriverObject, RegistryPath);
    return STATUS_SUCCESS;
}


/*
 * @unimplemented
 */
STORPORT_API
PUCHAR
NTAPI
StorPortAllocateRegistryBuffer(
    _In_ PVOID HwDeviceExtension,
    _In_ PULONG Length)
{
    DPRINT1("StorPortAllocateRegistryBuffer()\n");
    UNIMPLEMENTED;
    return NULL;
}


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortBusy(
    _In_ PVOID HwDeviceExtension,
    _In_ ULONG RequestsToComplete)
{
    DPRINT1("StorPortBuzy()\n");
    UNIMPLEMENTED;
    return FALSE;
}


/*
 * @unimplemented
 */
STORPORT_API
VOID
NTAPI
StorPortCompleteRequest(
    _In_ PVOID HwDeviceExtension,
    _In_ UCHAR PathId,
    _In_ UCHAR TargetId,
    _In_ UCHAR Lun,
    _In_ UCHAR SrbStatus)
{
    DPRINT1("StorPortCompleteRequest()\n");
    UNIMPLEMENTED;
}


/*
 * @implemented
 */
STORPORT_API
ULONG
NTAPI
StorPortConvertPhysicalAddressToUlong(
    _In_ STOR_PHYSICAL_ADDRESS Address)
{
    DPRINT1("StorPortConvertPhysicalAddressToUlong()\n");

    return Address.u.LowPart;
}


/*
 * @implemented
 */
STORPORT_API
STOR_PHYSICAL_ADDRESS
NTAPI
StorPortConvertUlongToPhysicalAddress(
    _In_ ULONG_PTR UlongAddress)
{
    STOR_PHYSICAL_ADDRESS Address;

    DPRINT1("StorPortConvertUlongToPhysicalAddress()\n");

    Address.QuadPart = UlongAddress;
    return Address;
}


/*
 * @implemented
 */
STORPORT_API
VOID
StorPortDebugPrint(
    _In_ ULONG DebugPrintLevel,
    _In_ PCHAR DebugMessage,
    ...)
{
    va_list ap;

    va_start(ap, DebugMessage);
    vDbgPrintExWithPrefix("STORMINI: ", 0x58, DebugPrintLevel, DebugMessage, ap);
    va_end(ap);
}


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortDeviceBusy(
    _In_ PVOID HwDeviceExtension,
    _In_ UCHAR PathId,
    _In_ UCHAR TargetId,
    _In_ UCHAR Lun,
    _In_ ULONG RequestsToComplete)
{
    DPRINT1("StorPortDeviceBusy()\n");
    UNIMPLEMENTED;
    return FALSE;
}


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortDeviceReady(
    _In_ PVOID HwDeviceExtension,
    _In_ UCHAR PathId,
    _In_ UCHAR TargetId,
    _In_ UCHAR Lun)
{
    DPRINT1("StorPortDeviceReady()\n");
    UNIMPLEMENTED;
    return FALSE;
}


/*
 * @unimplemented
 */
STORPORT_API
ULONG
StorPortExtendedFunction(
    _In_ STORPORT_FUNCTION_CODE FunctionCode,
    _In_ PVOID HwDeviceExtension,
    ...)
{
    DPRINT1("StorPortExtendedFunction(%d %p ...)\n",
            FunctionCode, HwDeviceExtension);
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}


/*
 * @implemented
 */
STORPORT_API
VOID
NTAPI
StorPortFreeDeviceBase(
    _In_ PVOID HwDeviceExtension,
    _In_ PVOID MappedAddress)
{
    DPRINT1("StorPortFreeDeviceBase(%p %p)\n",
            HwDeviceExtension, MappedAddress);
}


/*
 * @unimplemented
 */
STORPORT_API
VOID
NTAPI
StorPortFreeRegistryBuffer(
    _In_ PVOID HwDeviceExtension,
    _In_ PUCHAR Buffer)
{
    DPRINT1("StorPortFreeRegistryBuffer()\n");
    UNIMPLEMENTED;
}


/*
 * @implemented
 */
STORPORT_API
ULONG
NTAPI
StorPortGetBusData(
    _In_ PVOID DeviceExtension,
    _In_ ULONG BusDataType,
    _In_ ULONG SystemIoBusNumber,
    _In_ ULONG SlotNumber,
    _Out_ _When_(Length != 0, _Out_writes_bytes_(Length)) PVOID Buffer,
    _In_ ULONG Length)
{
    PMINIPORT_DEVICE_EXTENSION MiniportExtension;
    PBUS_INTERFACE_STANDARD Interface;
    ULONG ReturnLength;

    DPRINT1("StorPortGetBusData(%p %lu %lu %lu %p %lu)\n",
            DeviceExtension, BusDataType, SystemIoBusNumber, SlotNumber, Buffer, Length);

    /* Get the miniport extension */
    MiniportExtension = CONTAINING_RECORD(DeviceExtension,
                                          MINIPORT_DEVICE_EXTENSION,
                                          HwDeviceExtension);
    DPRINT1("DeviceExtension %p  MiniportExtension %p\n",
            DeviceExtension, MiniportExtension);

    Interface = &MiniportExtension->Miniport->DeviceExtension->BusInterface;

    if (BusDataType == 4)
        BusDataType = 0;

    ReturnLength = Interface->GetBusData(Interface->Context,
                                         BusDataType,
                                         Buffer,
                                         0,
                                         Length);
    DPRINT1("ReturnLength: %lu\n", ReturnLength);

    return ReturnLength;
}


/*
 * @implemented
 */
STORPORT_API
PVOID
NTAPI
StorPortGetDeviceBase(
    _In_ PVOID HwDeviceExtension,
    _In_ INTERFACE_TYPE BusType,
    _In_ ULONG SystemIoBusNumber,
    _In_ STOR_PHYSICAL_ADDRESS IoAddress,
    _In_ ULONG NumberOfBytes,
    _In_ BOOLEAN InIoSpace)
{
    PMINIPORT_DEVICE_EXTENSION MiniportExtension;
    PHYSICAL_ADDRESS TranslatedAddress;
    PVOID MappedAddress;
    NTSTATUS Status;

    DPRINT1("StorPortGetDeviceBase(%p %lu %lu 0x%I64x %lu %u)\n",
            HwDeviceExtension, BusType, SystemIoBusNumber, IoAddress.QuadPart, NumberOfBytes, InIoSpace);

    /* Get the miniport extension */
    MiniportExtension = CONTAINING_RECORD(HwDeviceExtension,
                                          MINIPORT_DEVICE_EXTENSION,
                                          HwDeviceExtension);
    DPRINT1("HwDeviceExtension %p  MiniportExtension %p\n",
            HwDeviceExtension, MiniportExtension);

    if (!TranslateResourceListAddress(MiniportExtension->Miniport->DeviceExtension,
                                      BusType,
                                      SystemIoBusNumber,
                                      IoAddress,
                                      NumberOfBytes,
                                      InIoSpace,
                                      &TranslatedAddress))
    {
        DPRINT1("Checkpoint!\n");
        return NULL;
    }

    DPRINT1("Translated Address: 0x%I64x\n", TranslatedAddress.QuadPart);

    /* In I/O space */
    if (InIoSpace)
    {
        DPRINT1("Translated Address: %p\n", (PVOID)(ULONG_PTR)TranslatedAddress.QuadPart);
        return (PVOID)(ULONG_PTR)TranslatedAddress.QuadPart;
    }

    /* In memory space */
    MappedAddress = MmMapIoSpace(TranslatedAddress,
                                 NumberOfBytes,
                                 FALSE);
    DPRINT1("Mapped Address: %p\n", MappedAddress);

    Status = AllocateAddressMapping(&MiniportExtension->Miniport->DeviceExtension->MappedAddressList,
                                    IoAddress,
                                    MappedAddress,
                                    NumberOfBytes,
                                    SystemIoBusNumber);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Checkpoint!\n");
        MappedAddress = NULL;
    }

    DPRINT1("Mapped Address: %p\n", MappedAddress);
    return MappedAddress;
}


/*
 * @unimplemented
 */
STORPORT_API
PVOID
NTAPI
StorPortGetLogicalUnit(
    _In_ PVOID HwDeviceExtension,
    _In_ UCHAR PathId,
    _In_ UCHAR TargetId,
    _In_ UCHAR Lun)
{
    DPRINT1("StorPortGetLogicalUnit()\n");
    UNIMPLEMENTED;
    return NULL;
}


/*
 * @implemented
 */
STORPORT_API
STOR_PHYSICAL_ADDRESS
NTAPI
StorPortGetPhysicalAddress(
    _In_ PVOID HwDeviceExtension,
    _In_opt_ PSCSI_REQUEST_BLOCK Srb,
    _In_ PVOID VirtualAddress,
    _Out_ ULONG *Length)
{
    PMINIPORT_DEVICE_EXTENSION MiniportExtension;
    PFDO_DEVICE_EXTENSION DeviceExtension;
    STOR_PHYSICAL_ADDRESS PhysicalAddress;
    ULONG_PTR Offset;

    DPRINT1("StorPortGetPhysicalAddress(%p %p %p %p)\n",
            HwDeviceExtension, Srb, VirtualAddress, Length);

    /* Get the miniport extension */
    MiniportExtension = CONTAINING_RECORD(HwDeviceExtension,
                                          MINIPORT_DEVICE_EXTENSION,
                                          HwDeviceExtension);
    DPRINT1("HwDeviceExtension %p  MiniportExtension %p\n",
            HwDeviceExtension, MiniportExtension);

    DeviceExtension = MiniportExtension->Miniport->DeviceExtension;

    /* Inside of the uncached extension? */
    if (((ULONG_PTR)VirtualAddress >= (ULONG_PTR)DeviceExtension->UncachedExtensionVirtualBase) &&
        ((ULONG_PTR)VirtualAddress <= (ULONG_PTR)DeviceExtension->UncachedExtensionVirtualBase + DeviceExtension->UncachedExtensionSize))
    {
        Offset = (ULONG_PTR)VirtualAddress - (ULONG_PTR)DeviceExtension->UncachedExtensionVirtualBase;

        PhysicalAddress.QuadPart = DeviceExtension->UncachedExtensionPhysicalBase.QuadPart + Offset;
        *Length = DeviceExtension->UncachedExtensionSize - Offset;

        return PhysicalAddress;
    }

    /*
     * Srb->DataBuffer is typically MmGetMdlVirtualAddress(Irp->MdlAddress),
     * a process-dependent VA that is NOT valid in whatever process context
     * HwStartIo runs in (especially for paging I/O). Calling
     * MmGetPhysicalAddress on such a VA returns garbage or zero, causing
     * DMA to the wrong physical page or a zero-address rejection.
     *
     * Instead, translate through the MDL's PFN array when the address falls
     * within the SRB's data buffer range.
     */
    if (Srb != NULL && Srb->DataBuffer != NULL &&
        (ULONG_PTR)VirtualAddress >= (ULONG_PTR)Srb->DataBuffer &&
        (ULONG_PTR)VirtualAddress <
        (ULONG_PTR)Srb->DataBuffer + Srb->DataTransferLength)
    {
        ULONG_PTR Offset = (ULONG_PTR)VirtualAddress - (ULONG_PTR)Srb->DataBuffer;
        ULONG PageIndex = Offset >> PAGE_SHIFT;
        ULONG PageOffset = Offset & (PAGE_SIZE - 1);

        /* Access the IRP behind the Srb via the miniport extension's
         * saved Irp pointer. ReactOS storport saves it in the LU extension.
         * For now, use the physical address of the system-mapped buffer if
         * the VA happens to be system-wide valid; otherwise report failure.
         */
        PhysicalAddress = MmGetPhysicalAddress(VirtualAddress);
        if (PhysicalAddress.QuadPart != 0)
        {
            *Length = PAGE_SIZE - PageOffset;
            if (Srb->DataTransferLength - Offset < *Length)
                *Length = Srb->DataTransferLength - (ULONG)Offset;
            return PhysicalAddress;
        }

        /* VA not mapped in current context: report zero so the miniport
         * can reject this SG element rather than programming garbage DMA. */
        PhysicalAddress.QuadPart = 0;
        *Length = 0;
        return PhysicalAddress;
    }

    /* Non-SRB addresses (uncached extension etc.) are always system VAs */
    PhysicalAddress = MmGetPhysicalAddress(VirtualAddress);
    *Length = PAGE_SIZE - ((ULONG_PTR)VirtualAddress & (PAGE_SIZE - 1));

    return PhysicalAddress;
}


/*
 * @unimplemented
 */
STORPORT_API
PSTOR_SCATTER_GATHER_LIST
NTAPI
StorPortGetScatterGatherList(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    DPRINT1("StorPortGetScatterGatherList()\n");
    UNIMPLEMENTED;
    return NULL;
}


/*
 * @implemented
 */
STORPORT_API
PSCSI_REQUEST_BLOCK
NTAPI
StorPortGetSrb(
    _In_ PVOID DeviceExtension,
    _In_ UCHAR PathId,
    _In_ UCHAR TargetId,
    _In_ UCHAR Lun,
    _In_ LONG QueueTag)
{
    DPRINT("StorPortGetSrb()\n");
    return NULL;
}


/*
 * @implemented
 */
STORPORT_API
PVOID
NTAPI
StorPortGetUncachedExtension(
    _In_ PVOID HwDeviceExtension,
    _In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _In_ ULONG NumberOfBytes)
{
    PMINIPORT_DEVICE_EXTENSION MiniportExtension;
    PFDO_DEVICE_EXTENSION DeviceExtension;
    PHYSICAL_ADDRESS LowestAddress, HighestAddress, Alignment;

    DPRINT1("StorPortGetUncachedExtension(%p %p %lu)\n",
            HwDeviceExtension, ConfigInfo, NumberOfBytes);

    /* Get the miniport extension */
    MiniportExtension = CONTAINING_RECORD(HwDeviceExtension,
                                          MINIPORT_DEVICE_EXTENSION,
                                          HwDeviceExtension);
    DPRINT1("HwDeviceExtension %p  MiniportExtension %p\n",
            HwDeviceExtension, MiniportExtension);

    DeviceExtension = MiniportExtension->Miniport->DeviceExtension;

    /* Return the uncached extension base address if we already have one */
    if (DeviceExtension->UncachedExtensionVirtualBase != NULL)
        return DeviceExtension->UncachedExtensionVirtualBase;

    // FIXME: Set DMA stuff here?

    /* Allocate the uncached extension */
    Alignment.QuadPart = 0;
    LowestAddress.QuadPart = 0;
    HighestAddress.QuadPart = 0x00000000FFFFFFFF;
    DeviceExtension->UncachedExtensionVirtualBase = MmAllocateContiguousMemorySpecifyCache(NumberOfBytes,
                                                                                           LowestAddress,
                                                                                           HighestAddress,
                                                                                           Alignment,
                                                                                           MmCached);
    if (DeviceExtension->UncachedExtensionVirtualBase == NULL)
        return NULL;

    DeviceExtension->UncachedExtensionPhysicalBase = MmGetPhysicalAddress(DeviceExtension->UncachedExtensionVirtualBase);
    DeviceExtension->UncachedExtensionSize = NumberOfBytes;

    return DeviceExtension->UncachedExtensionVirtualBase;
}


/*
 * @unimplemented
 */
STORPORT_API
PVOID
NTAPI
StorPortGetVirtualAddress(
    _In_ PVOID HwDeviceExtension,
    _In_ STOR_PHYSICAL_ADDRESS PhysicalAddress)
{
    DPRINT1("StorPortGetVirtualAddress(%p %I64x)\n",
            HwDeviceExtension, PhysicalAddress.QuadPart);
    UNIMPLEMENTED;
    return NULL;
}


/*
 * @implemented
 */
STORPORT_API
ULONG
NTAPI
StorPortInitialize(
    _In_ PVOID Argument1,
    _In_ PVOID Argument2,
    _In_ struct _HW_INITIALIZATION_DATA *HwInitializationData,
    _In_opt_ PVOID HwContext)
{
    PDRIVER_OBJECT DriverObject = (PDRIVER_OBJECT)Argument1;
    PUNICODE_STRING RegistryPath = (PUNICODE_STRING)Argument2;
    PDRIVER_OBJECT_EXTENSION DriverObjectExtension;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT1("StorPortInitialize(%p %p %p %p)\n",
            Argument1, Argument2, HwInitializationData, HwContext);

    DPRINT1("HwInitializationDataSize: %lu\n", HwInitializationData->HwInitializationDataSize);
    DPRINT1("AdapterInterfaceType: %u\n", HwInitializationData->AdapterInterfaceType);
    DPRINT1("HwInitialize: %p\n", HwInitializationData->HwInitialize);
    DPRINT1("HwStartIo: %p\n", HwInitializationData->HwStartIo);
    DPRINT1("HwInterrupt: %p\n", HwInitializationData->HwInterrupt);
    DPRINT1("HwFindAdapter: %p\n", HwInitializationData->HwFindAdapter);
    DPRINT1("HwResetBus: %p\n", HwInitializationData->HwResetBus);
    DPRINT1("HwDmaStarted: %p\n", HwInitializationData->HwDmaStarted);
    DPRINT1("HwAdapterState: %p\n", HwInitializationData->HwAdapterState);
    DPRINT1("DeviceExtensionSize: %lu\n", HwInitializationData->DeviceExtensionSize);
    DPRINT1("SpecificLuExtensionSize: %lu\n", HwInitializationData->SpecificLuExtensionSize);
    DPRINT1("SrbExtensionSize: %lu\n", HwInitializationData->SrbExtensionSize);
    DPRINT1("NumberOfAccessRanges: %lu\n", HwInitializationData->NumberOfAccessRanges);

    /* Check parameters */
    if ((DriverObject == NULL) ||
        (RegistryPath == NULL) ||
        (HwInitializationData == NULL))
    {
        DPRINT1("Invalid parameter!\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Check initialization data */
    if ((HwInitializationData->HwInitializationDataSize < sizeof(HW_INITIALIZATION_DATA)) ||
        (HwInitializationData->HwInitialize == NULL) ||
        (HwInitializationData->HwStartIo == NULL) ||
        (HwInitializationData->HwFindAdapter == NULL) ||
        (HwInitializationData->HwResetBus == NULL))
    {
        DPRINT1("Revision mismatch!\n");
        return STATUS_REVISION_MISMATCH;
    }

    DriverObjectExtension = IoGetDriverObjectExtension(DriverObject,
                                                       (PVOID)DriverEntry);
    if (DriverObjectExtension == NULL)
    {
        DPRINT1("No driver object extension!\n");

        Status = IoAllocateDriverObjectExtension(DriverObject,
                                                 (PVOID)DriverEntry,
                                                 sizeof(DRIVER_OBJECT_EXTENSION),
                                                 (PVOID *)&DriverObjectExtension);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("IoAllocateDriverObjectExtension() failed (Status 0x%08lx)\n", Status);
            return Status;
        }

        DPRINT1("Driver object extension created!\n");

        /* Initialize the driver object extension */
        RtlZeroMemory(DriverObjectExtension,
                      sizeof(DRIVER_OBJECT_EXTENSION));

        DriverObjectExtension->ExtensionType = DriverExtension;
        DriverObjectExtension->DriverObject = DriverObject;

        InitializeListHead(&DriverObjectExtension->AdapterListHead);
        KeInitializeSpinLock(&DriverObjectExtension->AdapterListLock);

        InitializeListHead(&DriverObjectExtension->InitDataListHead);

        /* Set handlers */
        DriverObject->DriverExtension->AddDevice = PortAddDevice;
//        DriverObject->DriverStartIo = PortStartIo;
        DriverObject->DriverUnload = PortUnload;
        DriverObject->MajorFunction[IRP_MJ_CREATE] = PortDispatchCreate;
        DriverObject->MajorFunction[IRP_MJ_CLOSE] = PortDispatchClose;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = PortDispatchDeviceControl;
        DriverObject->MajorFunction[IRP_MJ_SCSI] = PortDispatchScsi;
        DriverObject->MajorFunction[IRP_MJ_POWER] = PortDispatchPower;
        DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = PortDispatchSystemControl;
        DriverObject->MajorFunction[IRP_MJ_PNP] = PortDispatchPnp;
    }

    /* Add the initialzation data to the driver extension */
    Status = PortAddDriverInitData(DriverObjectExtension,
                                   HwInitializationData);

    DPRINT1("StorPortInitialize() done (Status 0x%08lx)\n", Status);

    return Status;
}


/*
 * @unimplemented
 */
STORPORT_API
VOID
NTAPI
StorPortLogError(
    _In_ PVOID HwDeviceExtension,
    _In_opt_ PSCSI_REQUEST_BLOCK Srb,
    _In_ UCHAR PathId,
    _In_ UCHAR TargetId,
    _In_ UCHAR Lun,
    _In_ ULONG ErrorCode,
    _In_ ULONG UniqueId)
{
    DPRINT1("ScsiPortLogError() called\n");
    DPRINT1("PathId: 0x%02x  TargetId: 0x%02x  Lun: 0x%02x  ErrorCode: 0x%08lx  UniqueId: 0x%08lx\n",
            PathId, TargetId, Lun, ErrorCode, UniqueId);

    DPRINT1("ScsiPortLogError() done\n");
}


/*
 * @implemented
 */
STORPORT_API
VOID
NTAPI
StorPortMoveMemory(
    _Out_writes_bytes_(Length) PVOID Destination,
    _In_reads_bytes_(Length) PVOID Source,
    _In_ ULONG Length)
{
    RtlMoveMemory(Destination, Source, Length);
}


typedef struct _PORT_COMPLETION_WORKER
{
    PIO_WORKITEM WorkItem;
    PDEVICE_OBJECT DeviceObject;
} PORT_COMPLETION_WORKER, *PPORT_COMPLETION_WORKER;

static
VOID
NTAPI
PortCompletionWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PFDO_DEVICE_EXTENSION DeviceExtension;
    PSLIST_ENTRY Entry;
    PPORT_COMPLETION_WORKER Worker = Context;

    DeviceExtension = (PFDO_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    /* Passive level: classpnp's completion path is paged code, so IRPs must
     * not be completed from the DPC itself. */
    while ((Entry = InterlockedPopEntrySList(&DeviceExtension->CompletionList)) != NULL)
    {
        PSTOR_REQUEST_CONTEXT Request;
        PIRP Irp;
        NTSTATUS Status;
        ULONG_PTR Information;

        Request = CONTAINING_RECORD(Entry, STOR_REQUEST_CONTEXT, ListEntry);
        Irp = Request->Irp;
        Status = Request->Status;
        Information = Request->Information;

        DPRINT1("PortCompletionWorker: ext %p ctx %p entry %p irp %p status %lx\n",
                DeviceExtension, Request, Entry, Irp, Status);

        /* Complete the IRP but KEEP the context block: classpnp caches
         * Srb->SrbExtension (which points inside it) in its per-device state
         * and may write through that pointer long after IoCompleteRequest.
         * fdo.c frees this block when the NEXT request is admitted. */
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = Information;
        IoCompleteRequest(Irp, IO_DISK_INCREMENT);

        /* Recycle the block for the next request; classpnp may still hold
         * Srb->SrbExtension pointers into it (see fdo.c). */
        InterlockedExchangePointer(
            (PVOID volatile *)&DeviceExtension->CachedContext, Request);

     }

    IoFreeWorkItem(Worker->WorkItem);
    ExFreePoolWithTag(Worker, TAG_MINIPORT_DATA);
}

IO_ALLOCATION_ACTION
NTAPI
PortCompletionDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PFDO_DEVICE_EXTENSION DeviceExtension;
    PPORT_COMPLETION_WORKER Worker;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    DeviceExtension = (PFDO_DEVICE_EXTENSION)DeferredContext;
    if (DeviceExtension == NULL)
        return KeepObject;

    /* Hand off to a passive-level worker; classpnp completes IRPs in paged
     * code and cannot run at DISPATCH_LEVEL. */
    Worker = ExAllocatePoolWithTag(NonPagedPool,
                                   sizeof(PORT_COMPLETION_WORKER),
                                   TAG_MINIPORT_DATA);
    if (Worker == NULL)
        return KeepObject;

    Worker->DeviceObject = DeviceExtension->Device;
    Worker->WorkItem = IoAllocateWorkItem(DeviceExtension->Device);
    if (Worker->WorkItem == NULL)
    {
        ExFreePoolWithTag(Worker, TAG_MINIPORT_DATA);
        return KeepObject;
    }
    IoQueueWorkItem(Worker->WorkItem,
                    PortCompletionWorker,
                    CriticalWorkQueue,
                    Worker);
    return KeepObject;
}

/*
 * @unimplemented
 */
STORPORT_API
VOID
StorPortNotification(
    _In_ SCSI_NOTIFICATION_TYPE NotificationType,
    _In_ PVOID HwDeviceExtension,
    ...)
{
    PMINIPORT_DEVICE_EXTENSION MiniportExtension = NULL;
    PFDO_DEVICE_EXTENSION DeviceExtension = NULL;
    PHW_PASSIVE_INITIALIZE_ROUTINE HwPassiveInitRoutine;
    PSTORPORT_EXTENDED_FUNCTIONS *ppExtendedFunctions;
    PBOOLEAN Result;
    PSTOR_DPC Dpc;
    PHW_DPC_ROUTINE HwDpcRoutine;
    va_list ap;

    STOR_SPINLOCK SpinLock;
    PVOID LockContext;
    PSTOR_LOCK_HANDLE LockHandle;
    PSCSI_REQUEST_BLOCK Srb;

    DPRINT1("StorPortNotification(%x %p)\n",
            NotificationType, HwDeviceExtension);

    /* Get the miniport extension */
    if (HwDeviceExtension != NULL)
    {
        MiniportExtension = CONTAINING_RECORD(HwDeviceExtension,
                                              MINIPORT_DEVICE_EXTENSION,
                                              HwDeviceExtension);
        DPRINT1("HwDeviceExtension %p  MiniportExtension %p\n",
                HwDeviceExtension, MiniportExtension);

        DeviceExtension = MiniportExtension->Miniport->DeviceExtension;
    }

    va_start(ap, HwDeviceExtension);

    switch (NotificationType)
    {
        case RequestComplete:
            Srb = (PSCSI_REQUEST_BLOCK)va_arg(ap, PSCSI_REQUEST_BLOCK);
            DPRINT1("RequestComplete: Srb %p status %lx\n",
                    Srb, Srb->SrbStatus);
            if (DeviceExtension != NULL &&
                DeviceExtension->ExtensionType == FdoExtension)
            {
                PFDO_DEVICE_EXTENSION FdoExtension;
                PSTOR_REQUEST_CONTEXT Context;

                FdoExtension = (PFDO_DEVICE_EXTENSION)DeviceExtension;
                Context = (PSTOR_REQUEST_CONTEXT)InterlockedExchangePointer(
                    (PVOID volatile *)&FdoExtension->ActiveRequest, NULL);
                if (Context != NULL && Context->Srb == Srb)
                {
                    DPRINT1("RequestComplete: ext %p ctx %p srb %p irp %p irpType %lx\n",
                            FdoExtension, Context, Srb, Context->Irp,
                            (ULONG)(Context->Irp ? Context->Irp->Type : 0xDEAD));
                    Context->Status = (Srb->SrbStatus == SRB_STATUS_SUCCESS)
                                      ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
                    Context->Information = 0;
                    InterlockedPushEntrySList(&FdoExtension->CompletionList,
                                              &Context->ListEntry);
                    /* Completions can arrive from HwStartIo (miniport
                     * completes synchronously, no interrupt will fire) or
                     * from the miniport ISR. KeInsertQueueDpc is a no-op when
                     * already queued and the DPC drains the whole list, so an
                     * extra insert from either path is harmless. */
                    KeInsertQueueDpc(&FdoExtension->CompletionDpc, NULL, NULL);
                }
                else
                {
                    /* Not the active request (stale or foreign completion);
                       put the slot back if we took it. */
                    if (Context != NULL)
                    {
                        InterlockedCompareExchangePointer(
                            (PVOID volatile *)&FdoExtension->ActiveRequest,
                            Context, NULL);
                    }
                }
            }
            break;

        case GetExtendedFunctionTable:
            DPRINT1("GetExtendedFunctionTable\n");
            ppExtendedFunctions = (PSTORPORT_EXTENDED_FUNCTIONS*)va_arg(ap, PSTORPORT_EXTENDED_FUNCTIONS*);
            if (ppExtendedFunctions != NULL)
                *ppExtendedFunctions = NULL; /* FIXME */
            break;

        case EnablePassiveInitialization:
            DPRINT1("EnablePassiveInitialization\n");
            HwPassiveInitRoutine = (PHW_PASSIVE_INITIALIZE_ROUTINE)va_arg(ap, PHW_PASSIVE_INITIALIZE_ROUTINE);
            DPRINT1("HwPassiveInitRoutine %p\n", HwPassiveInitRoutine);
            Result = (PBOOLEAN)va_arg(ap, PBOOLEAN);

            *Result = FALSE;

            if ((DeviceExtension != NULL) &&
                (DeviceExtension->HwPassiveInitRoutine == NULL))
            {
                DeviceExtension->HwPassiveInitRoutine = HwPassiveInitRoutine;
                *Result = TRUE;
            }
            break;

        case InitializeDpc:
            DPRINT1("InitializeDpc\n");
            Dpc = (PSTOR_DPC)va_arg(ap, PSTOR_DPC);
            DPRINT1("Dpc %p\n", Dpc);
            HwDpcRoutine = (PHW_DPC_ROUTINE)va_arg(ap, PHW_DPC_ROUTINE);
            DPRINT1("HwDpcRoutine %p\n", HwDpcRoutine);

            KeInitializeDpc((PRKDPC)&Dpc->Dpc,
                            (PKDEFERRED_ROUTINE)HwDpcRoutine,
                            (PVOID)DeviceExtension);
            KeInitializeSpinLock(&Dpc->Lock);
            break;

        case IssueDpc:
            Dpc = (PSTOR_DPC)va_arg(ap, PSTOR_DPC);
            /* The miniport ISR defers its DPC; the port interrupt wrapper
             * queues it after MiniportHwInterrupt returns. */
            if (DeviceExtension != NULL &&
                DeviceExtension->ExtensionType == FdoExtension)
            {
                InterlockedExchangePointer(
                    (PVOID volatile *)&DeviceExtension->PendingMiniportDpc,
                    (PKDPC)&Dpc->Dpc);
                KeInsertQueueDpc((PRKDPC)&Dpc->Dpc, NULL, NULL);
            }
            break;

        case AcquireSpinLock:
            DPRINT1("AcquireSpinLock\n");
            SpinLock = (STOR_SPINLOCK)va_arg(ap, STOR_SPINLOCK);
            DPRINT1("SpinLock %lu\n", SpinLock);
            LockContext = (PVOID)va_arg(ap, PVOID);
            DPRINT1("LockContext %p\n", LockContext);
            LockHandle = (PSTOR_LOCK_HANDLE)va_arg(ap, PSTOR_LOCK_HANDLE);
            DPRINT1("LockHandle %p\n", LockHandle);
            PortAcquireSpinLock(DeviceExtension,
                                SpinLock,
                                LockContext,
                                LockHandle);
            break;

        case ReleaseSpinLock:
            DPRINT1("ReleaseSpinLock\n");
            LockHandle = (PSTOR_LOCK_HANDLE)va_arg(ap, PSTOR_LOCK_HANDLE);
            DPRINT1("LockHandle %p\n", LockHandle);
            PortReleaseSpinLock(DeviceExtension,
                                LockHandle);
            break;

        default:
            DPRINT1("Unsupported Notification %lx\n", NotificationType);
            break;
    }

    va_end(ap);
}


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortPause(
    _In_ PVOID HwDeviceExtension,
    _In_ ULONG TimeOut)
{
    DPRINT1("StorPortPause()\n");
    UNIMPLEMENTED;
    return FALSE;
}


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortPauseDevice(
    _In_ PVOID HwDeviceExtension,
    _In_ UCHAR PathId,
    _In_ UCHAR TargetId,
    _In_ UCHAR Lun,
    _In_ ULONG TimeOut)
{
    DPRINT1("StorPortPauseDevice()\n");
    UNIMPLEMENTED;
    return FALSE;
}


#if defined(_M_AMD64)
/*
 * @implemented
 */
/* KeQuerySystemTime is an inline function,
   so we cannot forward the export to ntoskrnl */
STORPORT_API
VOID
NTAPI
StorPortQuerySystemTime(
    _Out_ PLARGE_INTEGER CurrentTime)
{
    DPRINT1("StorPortQuerySystemTime(%p)\n", CurrentTime);

    KeQuerySystemTime(CurrentTime);
}
#endif /* defined(_M_AMD64) */


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortReady(
    _In_ PVOID HwDeviceExtension)
{
    DPRINT1("StorPortReady()\n");
    UNIMPLEMENTED;
    return FALSE;
}


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortRegistryRead(
    _In_ PVOID HwDeviceExtension,
    _In_ PUCHAR ValueName,
    _In_ ULONG Global,
    _In_ ULONG Type,
    _In_ PUCHAR Buffer,
    _In_ PULONG BufferLength)
{
    DPRINT1("StorPortRegistryRead()\n");
    UNIMPLEMENTED;
    return FALSE;
}


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortRegistryWrite(
    _In_ PVOID HwDeviceExtension,
    _In_ PUCHAR ValueName,
    _In_ ULONG Global,
    _In_ ULONG Type,
    _In_ PUCHAR Buffer,
    _In_ ULONG BufferLength)
{
    DPRINT1("StorPortRegistryWrite()\n");
    UNIMPLEMENTED;
    return FALSE;
}


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortResume(
    _In_ PVOID HwDeviceExtension)
{
    DPRINT1("StorPortResume()\n");
    UNIMPLEMENTED;
    return FALSE;
}


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortResumeDevice(
    _In_ PVOID HwDeviceExtension,
    _In_ UCHAR PathId,
    _In_ UCHAR TargetId,
    _In_ UCHAR Lun)
{
    DPRINT1("StorPortResumeDevice()\n");
    UNIMPLEMENTED;
    return FALSE;
}


/*
 * @implemented
 */
STORPORT_API
ULONG
NTAPI
StorPortSetBusDataByOffset(
    _In_ PVOID DeviceExtension,
    _In_ ULONG BusDataType,
    _In_ ULONG SystemIoBusNumber,
    _In_ ULONG SlotNumber,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length)
{
    PMINIPORT_DEVICE_EXTENSION MiniportExtension;
    PBUS_INTERFACE_STANDARD Interface;
    ULONG ReturnLength;

    DPRINT1("StorPortSetBusData(%p %lu %lu %lu %p %lu %lu)\n",
            DeviceExtension, BusDataType, SystemIoBusNumber, SlotNumber, Buffer, Offset, Length);

    MiniportExtension = CONTAINING_RECORD(DeviceExtension,
                                          MINIPORT_DEVICE_EXTENSION,
                                          HwDeviceExtension);
    DPRINT1("DeviceExtension %p  MiniportExtension %p\n",
            DeviceExtension, MiniportExtension);

    Interface = &MiniportExtension->Miniport->DeviceExtension->BusInterface;

    ReturnLength = Interface->SetBusData(Interface->Context,
                                         BusDataType,
                                         Buffer,
                                         Offset,
                                         Length);
    DPRINT1("ReturnLength: %lu\n", ReturnLength);

    return ReturnLength;
}


/*
 * @unimplemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortSetDeviceQueueDepth(
    _In_ PVOID HwDeviceExtension,
    _In_ UCHAR PathId,
    _In_ UCHAR TargetId,
    _In_ UCHAR Lun,
    _In_ ULONG Depth)
{
    DPRINT1("StorPortSetDeviceQueueDepth()\n");
    UNIMPLEMENTED;
    return FALSE;
}


/*
 * @implemented
 */
STORPORT_API
VOID
NTAPI
StorPortStallExecution(
    _In_ ULONG Delay)
{
    KeStallExecutionProcessor(Delay);
}


/*
 * @unimplemented
 */
STORPORT_API
VOID
NTAPI
StorPortSynchronizeAccess(
    _In_ PVOID HwDeviceExtension,
    _In_ PSTOR_SYNCHRONIZED_ACCESS SynchronizedAccessRoutine,
    _In_opt_ PVOID Context)
{
    DPRINT1("StorPortSynchronizeAccess()\n");
    UNIMPLEMENTED;
}


/*
 * @implemented
 */
STORPORT_API
BOOLEAN
NTAPI
StorPortValidateRange(
    _In_ PVOID HwDeviceExtension,
    _In_ INTERFACE_TYPE BusType,
    _In_ ULONG SystemIoBusNumber,
    _In_ STOR_PHYSICAL_ADDRESS IoAddress,
    _In_ ULONG NumberOfBytes,
    _In_ BOOLEAN InIoSpace)
{
    DPRINT1("StorPortValidateRange()\n");
    return TRUE;
}

/* EOF */
