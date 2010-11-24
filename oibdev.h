/*
 * This is the main header for the OpeniBoot driver.
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

#pragma warning(disable:4200)  // nameless struct/union
#pragma warning(disable:4201)  // nameless struct/union
#pragma warning(disable:4214)  // bit field types other than int
#include <initguid.h>
#include <ntddk.h>
#include "usbdi.h"
#include "usbdlib.h"
#include "driverspecs.h"

#pragma warning(default:4200)
#pragma warning(default:4201)
#pragma warning(default:4214)

#include <wdf.h>
#include <wdfusb.h>
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>

#ifndef _PRIVATE_H
#define _PRIVATE_H

// {7C10602A-59E3-4f8d-9A84-057610177D84}
static const GUID GUID_OIBDevice = 
{ 0x7c10602a, 0x59e3, 0x4f8d, { 0x9a, 0x84, 0x5, 0x76, 0x10, 0x17, 0x7d, 0x84 } };

#define POOL_TAG (ULONG) 'DBIO' // OIBD fourcc
#define _DRIVER_NAME_ "OIBDEV"

#define OIBDEV_BUFFER_SIZE (64*1024)
#define DEVICE_DESC_LENGTH 256

extern const __declspec(selectany) LONGLONG DEFAULT_CONTROL_TRANSFER_TIMEOUT = 5 * -1 * WDF_TIMEOUT_TO_SEC;

//
// Other defines
//
#define DEVICE_OBJECT_NAME_LENGTH           16
#define MAX_TO_SEND 0x80 //512

//
// A structure representing the instance information associated with
// this particular device.
//

typedef struct _DEVICE_CONTEXT {

    WDFUSBDEVICE        usbDevice;
    WDFUSBINTERFACE     usbInterface;

    WDFUSBPIPE          bulkIn;
    WDFUSBPIPE          bulkOut;
    
    WDFQUEUE            readQueue;
    WDFQUEUE            writeQueue;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext);

//
// Our Functions
//
DRIVER_INITIALIZE DriverEntry;

EVT_WDF_OBJECT_CONTEXT_CLEANUP oibdev_cleanup;

EVT_WDF_DRIVER_DEVICE_ADD oibdev_add;

EVT_WDF_DEVICE_PREPARE_HARDWARE oibdev_prepare;

EVT_WDF_DEVICE_FILE_CREATE oibdev_open;

EVT_WDF_FILE_CLOSE oibdev_close;

EVT_WDF_IO_QUEUE_IO_READ oibdev_read;

EVT_WDF_REQUEST_COMPLETION_ROUTINE oibdev_read_continue;

EVT_WDF_IO_QUEUE_IO_WRITE oibdev_write;

VOID oibdev_write_setup(__in WDFQUEUE _queue, __in WDFREQUEST _request);

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL oibdev_ioctl;

EVT_WDF_WORKITEM oibdev_read_setup;

EVT_WDF_REQUEST_COMPLETION_ROUTINE oibdev_read_complete;

EVT_WDF_REQUEST_COMPLETION_ROUTINE oibdev_write_complete;

EVT_WDF_USB_READER_COMPLETION_ROUTINE oibdev_interrupt;

EVT_WDF_IO_QUEUE_IO_STOP oibdev_stop;

EVT_WDF_REQUEST_CANCEL oibdev_cancel_read;

EVT_WDF_REQUEST_CANCEL oibdev_cancel_write;

EVT_WDF_DEVICE_D0_ENTRY oibdev_enter_D0;

EVT_WDF_DEVICE_D0_EXIT oibdev_exit_D0;

#endif

