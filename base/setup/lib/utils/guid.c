/*
 * PROJECT:     ReactOS Setup Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     GUID definitions (storage for the DEFINE_GUID constants)
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

/* Define INITGUID so that the DEFINE_GUID macros in diskguid.h emit
 * storage for the GPT partition type GUIDs (see also
 * base/system/diskpart/guid.c). */
#define INITGUID
#include <guiddef.h>
#include <diskguid.h>
