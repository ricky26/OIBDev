/*
 * This is the windows driver part of the OpeniBoot driver.
 *
 * Copyright (c) 2010 Ricky Taylor
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#include "oibdev.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, oibdev_cleanup)
#endif

NTSTATUS DriverEntry(
        __in PDRIVER_OBJECT _drvObj,
        __in PUNICODE_STRING _regPath
        )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS result;
    WDF_OBJECT_ATTRIBUTES attr;

    WDF_DRIVER_CONFIG_INIT(&config, oibdev_add);

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.EvtCleanupCallback = oibdev_cleanup;

    result = WdfDriverCreate(
            _drvObj,
            _regPath,
            NULL, // attr
            &config,
            WDF_NO_HANDLE
            );

    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to register driver (%d).\n", result);
        return result;
    }

    return result;
}

VOID oibdev_cleanup(WDFDRIVER _drv)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(_drv);
}
