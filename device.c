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
    //fileConfig.FileObjectClass = WdfFileObjectWdfCanUseFsContext;
    WdfDeviceInitSetFileObjectConfig(_devInit, &fileConfig, &attr);

    WdfDeviceInitSetExclusive(_devInit, TRUE);
    //WdfDeviceInitSetDeviceType(_devInit, FILE_DEVICE_SERIAL_PORT);

    result = WdfDeviceCreate(&_devInit, &attr, &dev);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to create device (0x%08x).\n", result);
        return result;
    }

    devCtx = GetDeviceContext(dev);
    devCtx->rxWaiting = 0;
	devCtx->readRequest = (WDFREQUEST)-1;
	devCtx->writeRequest = (WDFREQUEST)-1;
	devCtx->readyRequest = (WDFREQUEST)-1;
	devCtx->isReady = 0;

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
        case WdfUsbPipeTypeInterrupt:
            if(WdfUsbTargetPipeIsInEndpoint(pipe)) // is in EP
            {
                devCtx->intrIn = pipe;
                DbgPrint("OIB: Selected %d as interrupt in pipe (expected %d).\n",
                        pipeInfo.EndpointAddress, INTERRUPT_IN_ENDPOINT_INDEX);
            }
            else
            {
                devCtx->intrOut = pipe;
                DbgPrint("OIB: Selected %d as interrupt out pipe (expected %d).\n",
                        pipeInfo.EndpointAddress, INTERRUPT_OUT_ENDPOINT_INDEX);
            }
            break;

        case WdfUsbPipeTypeBulk:
            if(WdfUsbTargetPipeIsInEndpoint(pipe)) // is in EP
            {
                devCtx->bulkIn = pipe;
                DbgPrint("OIB: Selected %d as bulk in pipe (expected %d).\n",
                        pipeInfo.EndpointAddress, BULK_IN_ENDPOINT_INDEX);
            }
            else
            {
                devCtx->bulkOut = pipe;
                DbgPrint("OIB: Selected %d as bulk out pipe (expected %d).\n",
                        pipeInfo.EndpointAddress, BULK_OUT_ENDPOINT_INDEX);
            }
            break;
        }
    }

    if(!devCtx->intrIn
            || !devCtx->intrOut
            || !devCtx->bulkIn
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

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&readerConf,
            oibdev_interrupt,
            devCtx,
            sizeof(OpenIBootCommand));

    result = WdfUsbTargetPipeConfigContinuousReader(devCtx->intrIn, &readerConf);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIb: Failed to setup interrupt reader (0x%08x).\n", result);
        return result;
    }
    
    return result;
}

NTSTATUS oibdev_enter_D0(
    __in WDFDEVICE _dev,
    __in WDF_POWER_DEVICE_STATE _pState
    )
{
    NTSTATUS result = STATUS_SUCCESS;
    WDF_MEMORY_DESCRIPTOR md;
    OpenIBootCommand cmd;
    PDEVICE_CONTEXT devCtx = GetDeviceContext(_dev);

    DbgPrint("OIB: Enter D0.\n");

    result = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(devCtx->intrIn));
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to start interrupt queue (0x%08x).\n", result);
        return result;
    }

	cmd.command = OPENIBOOTCMD_ISREADY;
	cmd.dataLen = 0;

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, &cmd, sizeof(OpenIBootCommand));

	if(!NT_SUCCESS(WdfUsbTargetPipeWriteSynchronously(devCtx->intrOut, NULL, NULL, &md, NULL)))
		DbgPrint("OIB: Failed to send command (0x%08x).\n", result);
	else
		DbgPrint("OIB: Sent ISREADY command.\n");

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

    WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(devCtx->intrIn), WdfIoTargetLeaveSentIoPending);

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
    WDFUSBPIPE pipe;
    WDFMEMORY memory;
    WDF_MEMORY_DESCRIPTOR md;
    WDFMEMORY_OFFSET offset;
    PDEVICE_CONTEXT devCtx;
    NTSTATUS result;
	int txAvail;

    devCtx = GetDeviceContext(WdfIoQueueGetDevice(_queue));

    if(devCtx->rxWaiting <= 0)
    {
        // No data to read, negotiate!
        OpenIBootCommand cmd;

        DbgPrint("OIB: Asking OpeniBoot for buffer size.\n");

        cmd.command = OPENIBOOTCMD_DUMPBUFFER;
        cmd.dataLen = 0;

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, &cmd, sizeof(OpenIBootCommand));

		devCtx->readRequest = _request;

        result = WdfUsbTargetPipeWriteSynchronously(devCtx->intrOut, NULL, NULL, &md, NULL);
        if(!NT_SUCCESS(result))
        {
            DbgPrint("OIB: Failed to send command (0x%08x).\n", result);
        }

        DbgPrint("OIB: Sent DUMPBUFFER command.\n");

		WdfRequestMarkCancelable(_request, oibdev_cancel_read);
        return;
    }

    pipe = devCtx->bulkIn;

    result = WdfRequestRetrieveOutputMemory(_request, &memory);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to allocate memory for read (0x%08x).\n", result);
        return;
    }

    WdfMemoryGetBuffer(memory, &_len); // Get the size of the buffer.
	if(_len > MAX_TO_SEND)
		_len = MAX_TO_SEND;

    if(devCtx->rxWaiting < _len)
		_len = devCtx->rxWaiting;
    
	devCtx->rxWaiting -= _len;
	
	offset.BufferOffset = 0;
	offset.BufferLength = _len;

	DbgPrint("OIB: Reading %d.\n", _len);

    result = WdfUsbTargetPipeFormatRequestForRead(pipe, _request, memory, &offset);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to setup read (0x%08x).\n", result);
        return;
    }

    WdfRequestSetCompletionRoutine(_request, oibdev_read_complete, pipe);

    DbgPrint("OIB: Sending USB read.\n");

    if(WdfRequestSend(_request, WdfUsbTargetPipeGetIoTarget(pipe), WDF_NO_SEND_OPTIONS) == FALSE)
    {
        result = WdfRequestGetStatus(_request);
        DbgPrint("OIB: Failed to send read request (0x%08x).\n", result);
        return;
    }
}

VOID oibdev_read_setup(
  __in  WDFWORKITEM _work
)
{
	WDFDEVICE dev = (WDFDEVICE)WdfWorkItemGetParentObject(_work);
	PDEVICE_CONTEXT devCtx = GetDeviceContext(dev);
	WDF_MEMORY_DESCRIPTOR md;
	OpenIBootCommand cmd;
	NTSTATUS result;
	
    cmd.command = OPENIBOOTCMD_DUMPBUFFER_GOAHEAD;
    cmd.dataLen = devCtx->rxWaiting;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, &cmd, sizeof(OpenIBootCommand));

    result = WdfUsbTargetPipeWriteSynchronously(devCtx->intrOut, NULL, NULL, &md, NULL);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to send command (0x%08x).\n", result);
    }

	DbgPrint("OIB: Sent DUMPBUFFER_GOAHEAD command.\n");
    
	WdfRequestCompleteWithInformation(devCtx->readRequest, result, 0);

	//oibdev_read(devCtx->readQueue, devCtx->readRequest, 0);
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
        DbgPrint("OIB: USB read failed (0x%08x).\n", result);

	devCtx->readRequest = (WDFREQUEST)-1;

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
    WDF_MEMORY_DESCRIPTOR md;
    OpenIBootCommand cmd;
    NTSTATUS result; 

    devCtx = GetDeviceContext(WdfIoQueueGetDevice(_queue));

    cmd.command = OPENIBOOTCMD_SENDCOMMAND;
    cmd.dataLen = _len;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, &cmd, sizeof(OpenIBootCommand));

	devCtx->writeRequest = _request;

    result = WdfUsbTargetPipeWriteSynchronously(devCtx->intrOut, NULL, NULL, &md, NULL);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to send command (0x%08x).\n", result);
    }

    DbgPrint("OIB: Sent SENDCOMMAND command.\n");

	WdfRequestMarkCancelable(_request, oibdev_cancel_write);
}

VOID oibdev_write_setup(__in WDFQUEUE _queue, __in WDFREQUEST _request)
{
    PDEVICE_CONTEXT devCtx;
    WDFUSBPIPE pipe;
    WDFMEMORY memory;
    NTSTATUS result; 

    devCtx = GetDeviceContext(WdfIoQueueGetDevice(_queue));

    pipe = devCtx->bulkOut;

    result = WdfRequestRetrieveInputMemory(_request, &memory);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to allocate memory for write (0x%08x).\n", result);
        return;
    }

    result = WdfUsbTargetPipeFormatRequestForWrite(pipe, _request, memory, NULL);
    if(!NT_SUCCESS(result))
    {
        DbgPrint("OIB: Failed to setup write (0x%08x).\n", result);
        return;
    }

	WdfRequestUnmarkCancelable(_request);
    WdfRequestSetCompletionRoutine(_request, oibdev_write_complete, pipe);

    if(WdfRequestSend(_request, WdfUsbTargetPipeGetIoTarget(pipe), WDF_NO_SEND_OPTIONS) == FALSE)
    {
        result = WdfRequestGetStatus(_request);
        DbgPrint("OIB: Failed to send write request (0x%08x).\n", result);
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

    if(!NT_SUCCESS(result))
        DbgPrint("OIB: USB write failed (0x%08x).\n", result);

	devCtx->writeRequest = (WDFREQUEST)-1;

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

VOID oibdev_interrupt(WDFUSBPIPE _pipe, WDFMEMORY _buffer, size_t _len, WDFCONTEXT _ctx)
{
    PDEVICE_CONTEXT devCtx = _ctx;
    WDFDEVICE dev;
    OpenIBootCommand *cmd;

    dev = WdfObjectContextGetObject(devCtx);

    if(_len != sizeof(OpenIBootCommand))
    {
        DbgPrint("OIB: Wrong-length interrupt! Got %d expected %d!\n", _len, sizeof(OpenIBootCommand));
    }
    else
    {
        cmd = WdfMemoryGetBuffer(_buffer, NULL);

        DbgPrint("OIB: Received OIB command %d: %d.\n", cmd->command, cmd->dataLen);

        RtlCopyMemory(&devCtx->lastCommand, cmd, sizeof(OpenIBootCommand));

        switch(cmd->command)
        {
        case OPENIBOOTCMD_DUMPBUFFER_LEN:
            {
                // We should now expect dataLen amount of buffer on the
                // in EP.
                //WDFREQUEST request;
				NTSTATUS result;

                DbgPrint("OIB: OpeniBoot has %d bytes to send!\n", cmd->dataLen);
                devCtx->rxWaiting = cmd->dataLen;
                
				if(devCtx->readRequest >= 0) //NT_SUCCESS(WdfIoQueueRetrieveNextRequest(devCtx->readQueue, &request)))
                {
					WdfRequestUnmarkCancelable(devCtx->readRequest);

					if(devCtx->rxWaiting <= 0)
						WdfRequestCompleteWithInformation(devCtx->readRequest, STATUS_SUCCESS, 0);
					else
					{
						WDF_OBJECT_ATTRIBUTES attr;
						WDF_WORKITEM_CONFIG workConfig;
						WDFWORKITEM work;

						WDF_OBJECT_ATTRIBUTES_INIT(&attr);
						attr.ParentObject = dev;

						WDF_WORKITEM_CONFIG_INIT(&workConfig, oibdev_read_setup);
						result = WdfWorkItemCreate(&workConfig, &attr, &work);
						if(!NT_SUCCESS(result))
						{
							DbgPrint("OIB: Failed to create read work (0x%08x).\n", result);
							WdfRequestComplete(devCtx->readRequest, STATUS_UNSUCCESSFUL);
							break;
						}

						WdfWorkItemEnqueue(work);
					}
                }
                else
                    DbgPrint("OIB: Nobody to handle receive.\n");
            }
            break;

        case OPENIBOOTCMD_SENDCOMMAND_GOAHEAD:
            {
                // We can now send data.
                WDFREQUEST request;

                DbgPrint("OIB: We can send!\n", cmd->dataLen);

                if(devCtx->writeRequest >= 0) //NT_SUCCESS(WdfIoQueueRetrieveNextRequest(devCtx->writeQueue, &request)))
                {
                    oibdev_write_setup(devCtx->writeQueue, devCtx->writeRequest); //request);
                }
                else
                    DbgPrint("OIB: Nobody to handle send.\n");

            }
            break;

        case OPENIBOOTCMD_READY:
            devCtx->isReady = 1;
			DbgPrint("OIB: Ready.\n");
            break;

        case OPENIBOOTCMD_NOTREADY:
            devCtx->isReady = 0;
			DbgPrint("OIB: Not ready.\n");
            break;
        }
    }
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
	case OIB_IOCTL_IS_READY:
		{
			WDFMEMORY mem;
			int *ptr;
			size_t size;

			status = WdfRequestRetrieveOutputMemory(_request, &mem);
			if(!NT_SUCCESS(status))
			{
				DbgPrint("OIB: Failed to get memory to store ISREADY output (0x%08x).\n", status);
				break;
			}

			ptr = (int*)WdfMemoryGetBuffer(mem, &size);
			if(ptr == NULL || size < sizeof(int))
			{
				DbgPrint("OIB: Memory not large enough to store output or ISREADY.\n");
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}

			*ptr = devCtx->isReady;
			bytesReturned = sizeof(int);
		}
		break;
    }

    WdfRequestCompleteWithInformation(_request, status, bytesReturned);
}
