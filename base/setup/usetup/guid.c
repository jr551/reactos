/*
 * PROJECT:     ReactOS text-mode setup
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     GUID definitions (storage for the DEFINE_GUID constants)
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

/* Define INITGUID so that the DEFINE_GUID macros in diskguid.h emit
 * storage for the GPT partition type GUIDs (see also
 * base/setup/lib/utils/guid.c and base/system/diskpart/guid.c). */
#define INITGUID
#include <guiddef.h>
#include <diskguid.h>
