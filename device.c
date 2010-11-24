/*
 * This is the device part of the OpeniBoot driver.
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
#include <ntddser.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, oibdev_add)
#pragma alloc_text(PAGE, oibdev_prepare)
#pragma alloc_text(PAGE, oibdev_ioctl)
#endif

NTSTATUS oibdev_add(WDFDRIVER _drv, PWDFDEVICE_INIT _devInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS powerCallbacks;
    WDF_DEVICE_PNP_CAPABILITIES pnpCaps;
    WDF_IO_QUEUE_CONFIG ioQueueConfig;
    WDF_FILEOBJECT_CONFIG fileConfig;
    PDEVICE_CONTEXT devCtx;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE dev;
    WDFQUEUE queue;
    NTSTATUS result;
    
    UNREFERENCED_PARAMETER(_drv);
    PAGED_CODE();

    DbgPrint("OIB: Adding device...\n");

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&powerCallbacks);
    powerCallbacks.EvtDevicePrepareHardware = oibdev_prepare;
    powerCallbacks.EvtDeviceD0Entry = oibdev_enter_D0;
    powerCallbacks.EvtDeviceD0Exit = oibdev_exit_D0;
    WdfDeviceInitSetPnpPowerEventCallbacks(_devInit, &powerCallbacks);

    WdfDeviceInitSetIoType(_devInit, WdfDeviceIoBuffered);

    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, oibdev_open, oibdev_close, NULL);
    WdfDeviceInitSetFileObjectConfig(_devInit, &fileConfig, &attr);

    WdfDeviceInitSetExclusive(_devInit, TRUE);
    WdfDeviceInitSetDeviceType(_devInit, FILE_DEVICE_SERIAL_PORT);

    result = WdfDeviceCreate(&_devInit, &attr, &dev);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to create device (0x%08x).\n", result);
        return result;
    }

    devCtx = GetDeviceContext(dev);

    // No need for safe removal
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
    pnpCaps.SurpriseRemovalOK = WdfTrue;
    WdfDeviceSetPnpCapabilities(dev, &pnpCaps);

    // Setup ioctl
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig, WdfIoQueueDispatchParallel);
    ioQueueConfig.EvtIoDeviceControl = oibdev_ioctl;
    result = WdfIoQueueCreate(dev, &ioQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to create IO queue (0x%08x).\n", result);
        return result;
    }

    // Setup read queue
    WDF_IO_QUEUE_CONFIG_INIT(&ioQueueConfig, WdfIoQueueDispatchSequential);
    ioQueueConfig.EvtIoRead = oibdev_read;
    ioQueueConfig.EvtIoStop = oibdev_stop;
    result = WdfIoQueueCreate(dev, &ioQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &devCtx->readQueue);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to create read queue (0x%08x).\n", result);
        return result;
    }

    result = WdfDeviceConfigureRequestDispatching(dev, devCtx->readQueue, WdfRequestTypeRead);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to setup read queue (0x%08x).\n", result);
        return result;
    }

    // Setup write
    WDF_IO_QUEUE_CONFIG_INIT(&ioQueueConfig, WdfIoQueueDispatchSequential);
    ioQueueConfig.EvtIoWrite = oibdev_write;
    ioQueueConfig.EvtIoStop = oibdev_stop;
    result = WdfIoQueueCreate(dev, &ioQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &devCtx->writeQueue);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to create write queue (0x%08x).\n", result);
        return result;
    }

    result = WdfDeviceConfigureRequestDispatching(dev, devCtx->writeQueue, WdfRequestTypeWrite);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to setup write queue (0x%08x).\n", result);
        return result;
    }

    // Expose driver interface
    result = WdfDeviceCreateDeviceInterface(dev, (LPGUID) &GUID_DEVINTERFACE_COMPORT, NULL);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to register interface (0x%08x).\n", result);
        return result;
    }
    
    DbgPrint("OIB: Device initialised.\n");

    return result;
}

NTSTATUS oibdev_prepare(WDFDEVICE _dev, WDFCMRESLIST _resList, WDFCMRESLIST _resListTrans)
{
    WDF_USB_DEVICE_INFORMATION devInfo;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
    WDF_USB_PIPE_INFORMATION pipeInfo;
    WDF_USB_CONTINUOUS_READER_CONFIG readerConf;
    WDFUSBPIPE pipe;
    PDEVICE_CONTEXT devCtx;
    ULONG waitWakeEnable;
    UCHAR index;
    UCHAR numPipes;
    NTSTATUS result;

    DECLARE_CONST_UNICODE_STRING(deviceName, L"\\Device\\OpeniBoot");
    DECLARE_CONST_UNICODE_STRING(linkName, L"\\DosDevices\\OpeniBoot");
    
    UNREFERENCED_PARAMETER(_resList);
    UNREFERENCED_PARAMETER(_resListTrans);

    PAGED_CODE();

    devCtx = GetDeviceContext(_dev);

    if(devCtx->usbDevice == NULL)
    {
        result = WdfUsbTargetDeviceCreate(_dev, WDF_NO_OBJECT_ATTRIBUTES, &devCtx->usbDevice);
        if(!NT_SUCCESS(result))
        {
            DbgPrint("OIB: failed to create target USB device (0x%08x).\n", result);
            return result;
        }
    }

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);

    result = WdfUsbTargetDeviceSelectConfig(devCtx->usbDevice, 
            WDF_NO_OBJECT_ATTRIBUTES, &configParams);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to select USB device configuration (0x%08x).\n", result);
        return result;
    }

    // Select the default interface. :P
    devCtx->usbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;
    numPipes = configParams.Types.SingleInterface.NumberConfiguredPipes;

    for(index=0; index < numPipes; index++)
    {
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

        pipe = WdfUsbInterfaceGetConfiguredPipe(devCtx->usbInterface, index, &pipeInfo);

        if(pipeInfo.EndpointAddress <= 0)
            continue;
        
		WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

        switch(pipeInfo.PipeType)
        {
		case WdfUsbPipeTypeBulk:
            if(WdfUsbTargetPipeIsInEndpoint(pipe)) // is in EP
            {
                devCtx->bulkIn = pipe;
                DbgPrint("OIB: Selected %d as bulk in pipe.\n", pipeInfo.EndpointAddress);
            }
            else
            {
                devCtx->bulkOut = pipe;
                DbgPrint("OIB: Selected %d as bulk out pipe.\n", pipeInfo.EndpointAddress);
            }
            break;
        }
    }

    if(!devCtx->bulkIn
       || !devCtx->bulkOut)
    {
        DbgPrint("OIB: Failed to assign all pipes.\n");
        result = STATUS_INVALID_DEVICE_STATE;
        return result;
    }

    result = WdfDeviceCreateSymbolicLink(_dev, &deviceName);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to create OiB device symlink (0x%08x).\n", result);
        return result;
    }

    if(!NT_SUCCESS(IoCreateSymbolicLink((PUNICODE_STRING)&linkName, (PUNICODE_STRING)&deviceName)))
        DbgPrint("OIB: Failed to add DosDevices symlink.\n");
    
    return result;
}

NTSTATUS oibdev_enter_D0(
    __in WDFDEVICE _dev,
    __in WDF_POWER_DEVICE_STATE _pState
    )
{
    NTSTATUS result = STATUS_SUCCESS;
    PDEVICE_CONTEXT devCtx = GetDeviceContext(_dev);

    DbgPrint("OIB: Enter D0.\n");
	
	return result;
}

NTSTATUS oibdev_exit_D0(
    __in WDFDEVICE _dev,
    __in WDF_POWER_DEVICE_STATE _targetState
)
{
    NTSTATUS result = STATUS_SUCCESS;
    PDEVICE_CONTEXT devCtx = GetDeviceContext(_dev);
    
    DbgPrint("OIB: Exit D0.\n");

    return result;
}

VOID oibdev_open(
    __in  WDFDEVICE _dev,
    __in  WDFREQUEST _request,
    __in  WDFFILEOBJECT _file
)
{
    DbgPrint("OIB: Opened.\n");
    WdfRequestComplete(_request, STATUS_SUCCESS);
}

VOID oibdev_close(
  __in  WDFFILEOBJECT _file
)
{
    DbgPrint("OIB: Closed.\n");
}

VOID oibdev_read(__in WDFQUEUE _queue, __in WDFREQUEST _request, size_t _len)
{
    WDFMEMORY memory;
    PDEVICE_CONTEXT devCtx;
    NTSTATUS result;

    devCtx = GetDeviceContext(WdfIoQueueGetDevice(_queue));

	DbgPrint("OIB: Reading %d.\n", _len);

    result = WdfRequestRetrieveOutputMemory(_request, &memory);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to allocate memory for read (0x%08x).\n", result);
		WdfRequestCompleteWithInformation(_request, result, 0);
        return;
    }

    result = WdfUsbTargetPipeFormatRequestForRead(devCtx->bulkIn, _request, memory, NULL);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to setup read (0x%08x).\n", result);
		WdfRequestCompleteWithInformation(_request, result, 0);
        return;
    }

    WdfRequestSetCompletionRoutine(_request, oibdev_read_complete, devCtx->bulkIn);

    if(WdfRequestSend(_request, WdfUsbTargetPipeGetIoTarget(devCtx->bulkIn), WDF_NO_SEND_OPTIONS) == FALSE)
    {
        result = WdfRequestGetStatus(_request);
        DbgPrint("OIB: Failed to send read request (0x%08x).\n", result);
		WdfRequestCompleteWithInformation(_request, result, 0);
		return;
    }
}

VOID oibdev_read_complete(
        __in WDFREQUEST _request,
        __in WDFIOTARGET _target,
        __in PWDF_REQUEST_COMPLETION_PARAMS _completionParams,
        __in WDFCONTEXT _context
        )
{
	WDFQUEUE queue = WdfRequestGetIoQueue(_request);
    PDEVICE_CONTEXT devCtx = GetDeviceContext(WdfIoQueueGetDevice(queue));
    PWDF_USB_REQUEST_COMPLETION_PARAMS usbCompletionParams;
    size_t bytesRead = 0;
    NTSTATUS result;

    UNREFERENCED_PARAMETER(_target);    
    UNREFERENCED_PARAMETER(_context);

    DbgPrint("OIB: Completing USB read.\n");

    result = _completionParams->IoStatus.Status;

    usbCompletionParams = _completionParams->Parameters.Usb.Completion;

    bytesRead = usbCompletionParams->Parameters.PipeRead.Length;

	DbgPrint("OIB: USBD status 0x%08x.\n", usbCompletionParams->UsbdStatus);

    if(!NT_SUCCESS(result))
        DbgPrint("OIB: USB read failed with %d bytes (0x%08x).\n", bytesRead, result);

    WdfRequestCompleteWithInformation(_request, result, bytesRead);
}

VOID oibdev_cancel_read(__in WDFREQUEST _request)
{
	DbgPrint("OIB: Cancelling read.\n");
	WdfRequestComplete(_request, STATUS_IO_TIMEOUT);
}

VOID oibdev_write(__in WDFQUEUE _queue, __in WDFREQUEST _request, size_t _len)
{
    PDEVICE_CONTEXT devCtx;
    WDFMEMORY memory;
    NTSTATUS result; 

    devCtx = GetDeviceContext(WdfIoQueueGetDevice(_queue));

	DbgPrint("OIB: Writing %d.\n", _len);

    result = WdfRequestRetrieveInputMemory(_request, &memory);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to allocate memory for write (0x%08x).\n", result);
		WdfRequestCompleteWithInformation(_request, result, 0);
        return;
    }

    result = WdfUsbTargetPipeFormatRequestForWrite(devCtx->bulkOut, _request, memory, NULL);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to setup write (0x%08x).\n", result);
		WdfRequestCompleteWithInformation(_request, result, 0);
        return;
    }

    WdfRequestSetCompletionRoutine(_request, oibdev_write_complete, devCtx->bulkOut);

    if(WdfRequestSend(_request, WdfUsbTargetPipeGetIoTarget(devCtx->bulkOut), WDF_NO_SEND_OPTIONS) == FALSE)
    {
        result = WdfRequestGetStatus(_request);
        DbgPrint("OIB: Failed to send write request (0x%08x).\n", result);
		WdfRequestCompleteWithInformation(_request, result, 0);
        return;
    }
}

VOID oibdev_write_complete(
        __in WDFREQUEST _request,
        __in WDFIOTARGET _target,
        __in PWDF_REQUEST_COMPLETION_PARAMS _completionParams,
        __in WDFCONTEXT _context
        )
{
	WDFQUEUE queue = WdfRequestGetIoQueue(_request);
    PDEVICE_CONTEXT devCtx = GetDeviceContext(WdfIoQueueGetDevice(queue));
    PWDF_USB_REQUEST_COMPLETION_PARAMS usbCompletionParams;
    size_t bytesRead = 0;
    NTSTATUS result;

    UNREFERENCED_PARAMETER(_target);    
    UNREFERENCED_PARAMETER(_context);    

    result = _completionParams->IoStatus.Status;

    usbCompletionParams = _completionParams->Parameters.Usb.Completion;

    bytesRead = usbCompletionParams->Parameters.PipeWrite.Length;

    DbgPrint("OIB: Completing USB write.\n");

    if(!NT_SUCCESS(result))
        DbgPrint("OIB: USB write failed (0x%08x).\n", result);

    WdfRequestCompleteWithInformation(_request, result, bytesRead);
}

VOID oibdev_cancel_write(__in WDFREQUEST _request)
{
	DbgPrint("OIB: Cancelling write.\n");
	WdfRequestComplete(_request, STATUS_IO_TIMEOUT);
}

VOID oibdev_stop(__in WDFQUEUE _queue, __in WDFREQUEST _request, __in ULONG _aFlags)
{
    UNREFERENCED_PARAMETER(_queue);

    if(_aFlags & WdfRequestStopActionSuspend)
        WdfRequestStopAcknowledge(_request, FALSE);
    else if(_aFlags & WdfRequestStopActionPurge)
        WdfRequestCancelSentRequest(_request);
}

VOID oibdev_ioctl(
        __in WDFQUEUE _queue,
        __in WDFREQUEST _request,
        __in size_t _outLen,
        __in size_t _inLen,
        __in ULONG _ioCode
        )
{
    WDFDEVICE dev;
    PDEVICE_CONTEXT devCtx;
    size_t bytesReturned = 0;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    UNREFERENCED_PARAMETER(_outLen);
    UNREFERENCED_PARAMETER(_inLen);

    PAGED_CODE();

    dev = WdfIoQueueGetDevice(_queue);
    devCtx = GetDeviceContext(dev);

    switch(_ioCode)
    {
		// IOCTLs go here if we ever need any -- Ricky26
    }

    WdfRequestCompleteWithInformation(_request, status, bytesReturned);
}
