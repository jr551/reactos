/*
 * Debug symbol provider for the shared virtio library.
 *
 * The static virtio library (sdk/lib/drivers/virtio) is linked into
 * viostor.sys and references the consumer-defined debug symbols declared
 * in kdebugprint.h. This file defines them for the miniport, mirroring
 * the provider pattern in netkvm's ParaNdis-Debug.c. Output also goes
 * to COM1 so Setup can be diagnosed without a KD session.
 */
#include <ntddk.h>
#include <stdarg.h>
#include <ntstrsafe.h>
#include <kdebugprint.h>

int virtioDebugLevel = 1;
int bDebugPrint = 1;

static void VioCom1Write(const char *Text)
{
    static BOOLEAN Initialized = FALSE;
    const char *Cursor;

    if (!Initialized) {
        WRITE_PORT_UCHAR((PUCHAR)0x3F9, 0x00);
        WRITE_PORT_UCHAR((PUCHAR)0x3FB, 0x80);
        WRITE_PORT_UCHAR((PUCHAR)0x3F8, 0x01);
        WRITE_PORT_UCHAR((PUCHAR)0x3F9, 0x00);
        WRITE_PORT_UCHAR((PUCHAR)0x3FB, 0x03);
        WRITE_PORT_UCHAR((PUCHAR)0x3FA, 0xC7);
        WRITE_PORT_UCHAR((PUCHAR)0x3FC, 0x0B);
        Initialized = TRUE;
    }
    for (Cursor = Text; *Cursor != '\0'; Cursor++) {
        ULONG Spin = 10000;
        while (Spin-- != 0 &&
               (READ_PORT_UCHAR((PUCHAR)0x3FD) & 0x20) == 0)
            ;
        WRITE_PORT_UCHAR((PUCHAR)0x3F8, (UCHAR)*Cursor);
        if (*Cursor == '\n') {
            Spin = 10000;
            while (Spin-- != 0 &&
                   (READ_PORT_UCHAR((PUCHAR)0x3FD) & 0x20) == 0)
                ;
            WRITE_PORT_UCHAR((PUCHAR)0x3F8, '\r');
        }
    }
}

static void __cdecl VirtioDebugPrint(const char *Format, ...)
{
    char Buffer[256];
    va_list ArgList;

    va_start(ArgList, Format);
    Buffer[0] = '\0';
    RtlStringCbVPrintfA(Buffer, sizeof(Buffer), Format, ArgList);
    va_end(ArgList);
    VioCom1Write(Buffer);
    /* Route through DbgPrint (KDBG) so output also reaches the serial
     * console under KVM where direct 0x3F8 writes from VioCom1Write
     * are not visible. This is the same path DPRINT1/storport uses. */
    DbgPrint("%s", Buffer);
}

tDebugPrintFunc VirtioDebugPrintProc = VirtioDebugPrint;
