/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Utils source
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/* GLOBALS ********************************************************************/

extern EFI_SYSTEM_TABLE *GlobalSystemTable;

/* FUNCTIONS ******************************************************************/

TIMEINFO*
UefiGetTime(VOID)
{
    static TIMEINFO TimeInfo;
    EFI_STATUS Status;
    EFI_TIME time = {0};

    Status = GlobalSystemTable->RuntimeServices->GetTime(&time, NULL);
    if (Status != EFI_SUCCESS)
        ERR("UefiGetTime: cannot get time status %d\n", Status);

    TimeInfo.Year = time.Year;
    TimeInfo.Month = time.Month;
    TimeInfo.Day = time.Day;
    TimeInfo.Hour = time.Hour;
    TimeInfo.Minute = time.Minute;
    TimeInfo.Second = time.Second;
    return &TimeInfo;
}

/*
 * Value of the "OsIndications" global variable requesting the firmware
 * to boot into its setup UI on the next boot (see the UEFI specification,
 * "OS Indications" chapter). Not defined in the in-tree edk2 headers.
 */
#define EFI_OS_INDICATIONS_BOOT_TO_FIRMWARE_UI 0x0000000000000001ULL

/**
 * @brief
 * Reboots the machine into the UEFI firmware setup UI, by setting the
 * "OsIndications" global variable and then performing a cold reset.
 * If the variable cannot be set, a plain cold reset is performed instead.
 **/
VOID
UefiRebootToFirmware(VOID)
{
    EFI_STATUS Status;
    EFI_GUID GlobalVariableGuid = EFI_GLOBAL_VARIABLE;
    UINT64 OsIndications = 0;
    UINTN Size = sizeof(OsIndications);

    /*
     * Read the current value of the "OsIndications" global variable and
     * OR in the BOOT_TO_FIRMWARE_UI bit, so that the existing indications
     * (if any) are preserved.
     */
    Status = GlobalSystemTable->RuntimeServices->GetVariable(
                 EFI_OS_INDICATIONS_VARIABLE_NAME,
                 &GlobalVariableGuid,
                 NULL,
                 &Size,
                 &OsIndications);
    if (EFI_ERROR(Status))
    {
        if (Status != EFI_NOT_FOUND)
        {
            ERR("UefiRebootToFirmware: failed to read the OsIndications variable (Status 0x%lx); assuming 0\n", Status);
        }
        OsIndications = 0;
    }
    OsIndications |= EFI_OS_INDICATIONS_BOOT_TO_FIRMWARE_UI;

    /* Ask the firmware to boot into its setup UI on the next boot */
    Status = GlobalSystemTable->RuntimeServices->SetVariable(
                 EFI_OS_INDICATIONS_VARIABLE_NAME,
                 &GlobalVariableGuid,
                 EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                 sizeof(OsIndications),
                 &OsIndications);
    if (EFI_ERROR(Status))
    {
        ERR("UefiRebootToFirmware: failed to set the OsIndications variable (Status 0x%lx); performing a plain cold reset\n", Status);
    }

    /* Cold-reset the machine */
    GlobalSystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);

    /* If we get here, the reset failed: loop forever as a safety net */
    for (;;)
    {
        NOTHING;
    }
}
