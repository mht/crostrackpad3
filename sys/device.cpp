#include "internal.h"
#include "device.h"
#include "hiddevice.h"
#include "spb.h"

//#include "device.tmh"

bool deviceLoaded = false;

/////////////////////////////////////////////////
//
// WDF callbacks.
//
/////////////////////////////////////////////////

NTSTATUS
OnPrepareHardware(
    _In_  WDFDEVICE     FxDevice,
    _In_  WDFCMRESLIST  FxResourcesRaw,
    _In_  WDFCMRESLIST  FxResourcesTranslated
    )
/*++
 
  Routine Description:

    This routine caches the SPB resource connection ID.

  Arguments:

    FxDevice - a handle to the framework device object
    FxResourcesRaw - list of translated hardware resources that 
        the PnP manager has assigned to the device
    FxResourcesTranslated - list of raw hardware resources that 
        the PnP manager has assigned to the device

  Return Value:

    Status

--*/
{
    FuncEntry(TRACE_FLAG_WDFLOADING);

    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
    BOOLEAN fSpbResourceFound = FALSE;
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

    UNREFERENCED_PARAMETER(FxResourcesRaw);

    //
    // Parse the peripheral's resources.
    //

    ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

    for(ULONG i = 0; i < resourceCount; i++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
        UCHAR Class;
        UCHAR Type;

        pDescriptor = WdfCmResourceListGetDescriptor(
            FxResourcesTranslated, i);

        switch (pDescriptor->Type)
        {
            case CmResourceTypeConnection:
                //
                // Look for I2C or SPI resource and save connection ID.
                //
                Class = pDescriptor->u.Connection.Class;
                Type = pDescriptor->u.Connection.Type;
                if (Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                    Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
                {
                    if (fSpbResourceFound == FALSE)
                    {
						status = STATUS_SUCCESS;
						pDevice->I2CContext.I2cResHubId.LowPart = pDescriptor->u.Connection.IdLowPart;
						pDevice->I2CContext.I2cResHubId.HighPart = pDescriptor->u.Connection.IdHighPart;
                        fSpbResourceFound = TRUE;
                        Trace(
                            TRACE_LEVEL_INFORMATION,
                            TRACE_FLAG_WDFLOADING,
                            "SPB resource found with ID=0x%llx",
							pDevice->I2CContext.I2cResHubId.QuadPart);
                    }
                    else
                    {
                        Trace(
                            TRACE_LEVEL_WARNING,
                            TRACE_FLAG_WDFLOADING,
                            "Duplicate SPB resource found with ID=0x%llx",
							pDevice->I2CContext.I2cResHubId.QuadPart);
                    }
                }
                break;
            default:
                //
                // Ignoring all other resource types.
                //
                break;
        }
    }

    //
    // An SPB resource is required.
    //

    if (fSpbResourceFound == FALSE)
    {
        status = STATUS_NOT_FOUND;
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_FLAG_WDFLOADING,
            "SPB resource not found - %!STATUS!", 
            status);
    }

	status = SpbTargetInitialize(FxDevice, &pDevice->I2CContext);
	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_FLAG_WDFLOADING,
			"Error in Spb initialization - %!STATUS!",
			status);

		return status;
	}

    FuncExit(TRACE_FLAG_WDFLOADING);

    return status;
}

NTSTATUS
OnReleaseHardware(
    _In_  WDFDEVICE     FxDevice,
    _In_  WDFCMRESLIST  FxResourcesTranslated
    )
/*++
 
  Routine Description:

  Arguments:

    FxDevice - a handle to the framework device object
    FxResourcesTranslated - list of raw hardware resources that 
        the PnP manager has assigned to the device

  Return Value:

    Status

--*/
{
    FuncEntry(TRACE_FLAG_WDFLOADING);
    
    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
    NTSTATUS status = STATUS_SUCCESS;
    
    UNREFERENCED_PARAMETER(FxResourcesTranslated);

	SpbTargetDeinitialize(FxDevice, &pDevice->I2CContext);

	deviceLoaded = false;

    FuncExit(TRACE_FLAG_WDFLOADING);

    return status;
}

bool IsCyapaLoaded(){
	return deviceLoaded;
}

void cyapa_set_power_mode(_In_  PDEVICE_CONTEXT  pDevice, _In_ uint8_t power_mode)
{
	int ret;
	uint8_t power;

	SpbReadDataSynchronously(&pDevice->I2CContext, CMD_POWER_MODE, &ret, 1);
	if (ret < 0)
		return;

	power = (ret & ~0xFC);
	power |= power_mode & 0xFc;

	SpbWriteDataSynchronously(&pDevice->I2CContext, CMD_POWER_MODE, &power, 1);
}

VOID
CyapaBootWorkItem(
	IN WDFWORKITEM  WorkItem
	)
{
	WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
	PDEVICE_CONTEXT pDevice = GetDeviceContext(Device);

	cyapa_boot_regs boot;

	csgesture_softc *sc = &pDevice->sc;

	if (!sc->infoSetup) {
		struct cyapa_cap cap;
		SpbReadDataSynchronously(&pDevice->I2CContext, CMD_QUERY_CAPABILITIES, &cap, sizeof(cap));
		if (strncmp((const char *)cap.prod_ida, "CYTRA", 5) != 0) {
			CyapaPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "[cyapainit] Product ID \"%5.5s\" mismatch\n",
				cap.prod_ida);
			SpbReadDataSynchronously(&pDevice->I2CContext, CMD_QUERY_CAPABILITIES, &cap, sizeof(cap));
		}

		sc->resx = ((cap.max_abs_xy_high << 4) & 0x0F00) |
			cap.max_abs_x_low;
		sc->resy = ((cap.max_abs_xy_high << 8) & 0x0F00) |
			cap.max_abs_y_low;
		sc->phyx = ((cap.phy_siz_xy_high << 4) & 0x0F00) |
			cap.phy_siz_x_low;
		sc->phyy = ((cap.phy_siz_xy_high << 8) & 0x0F00) |
			cap.phy_siz_y_low;
		CyapaPrint(DEBUG_LEVEL_INFO, DBG_PNP, "[cyapainit] %5.5s-%6.6s-%2.2s buttons=%c%c%c res=%dx%d\n",
			cap.prod_ida, cap.prod_idb, cap.prod_idc,
			((cap.buttons & CYAPA_FNGR_LEFT) ? 'L' : '-'),
			((cap.buttons & CYAPA_FNGR_MIDDLE) ? 'M' : '-'),
			((cap.buttons & CYAPA_FNGR_RIGHT) ? 'R' : '-'),
			sc->resx,
			sc->resy);

		for (int i = 0; i < 5; i++) {
			sc->product_id[i] = cap.prod_ida[i];
		}
		sc->product_id[5] = '-';
		for (int i = 0; i < 6; i++) {
			sc->product_id[i + 6] = cap.prod_idb[i];
		}
		sc->product_id[12] = '-';
		for (int i = 0; i < 2; i++) {
			sc->product_id[i + 13] = cap.prod_idc[i];
		}
		sc->product_id[15] = '\0';

		sprintf(sc->firmware_version, "%d.%d", cap.fw_maj_ver, cap.fw_min_ver);
		sc->infoSetup = true;
	}

	cyapa_set_power_mode(pDevice, CMD_POWER_MODE_FULL);

	SpbReadDataSynchronously(&pDevice->I2CContext, CMD_BOOT_STATUS, &boot, sizeof(boot));
	WdfObjectDelete(WorkItem);
}

void CyapaBootTimer(_In_ WDFTIMER hTimer) {
	WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(hTimer);
	PDEVICE_CONTEXT pDevice = GetDeviceContext(Device);

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_WORKITEM_CONFIG workitemConfig;
	WDFWORKITEM hWorkItem;

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
	attributes.ParentObject = Device;
	WDF_WORKITEM_CONFIG_INIT(&workitemConfig, CyapaBootWorkItem);

	WdfWorkItemCreate(&workitemConfig,
		&attributes,
		&hWorkItem);

	WdfWorkItemEnqueue(hWorkItem);
	WdfTimerStop(hTimer, FALSE);
}

NTSTATUS BOOTTRACKPAD(
	_In_  PDEVICE_CONTEXT  pDevice
	)
{
	NTSTATUS status = 0;

	static char bl_exit[] = {
		0x00, 0xff, 0xa5, 0x00, 0x01,
		0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };

	static char bl_deactivate[] = {
		0x00, 0xff, 0x3b, 0x00, 0x01,
		0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };

	cyapa_boot_regs boot;

	FuncEntry(TRACE_FLAG_WDFLOADING);

	SpbReadDataSynchronously(&pDevice->I2CContext, CMD_BOOT_STATUS, &boot, sizeof(boot));

	if ((boot.stat & CYAPA_STAT_RUNNING) == 0) {
		if (boot.error & CYAPA_ERROR_BOOTLOADER)
			SpbWriteDataSynchronously(&pDevice->I2CContext, CMD_BOOT_STATUS, bl_deactivate, sizeof(bl_deactivate));
		else
			SpbWriteDataSynchronously(&pDevice->I2CContext, CMD_BOOT_STATUS, bl_exit, sizeof(bl_exit));
	}

	WDF_TIMER_CONFIG              timerConfig;
	WDFTIMER                      hTimer;
	WDF_OBJECT_ATTRIBUTES         attributes;

	WDF_TIMER_CONFIG_INIT(&timerConfig, CyapaBootTimer);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = pDevice->FxDevice;
	status = WdfTimerCreate(&timerConfig, &attributes, &hTimer);

	WdfTimerStart(hTimer, WDF_REL_TIMEOUT_IN_MS(75));

	FuncExit(TRACE_FLAG_WDFLOADING);
	return status;
}

NTSTATUS
OnD0Entry(
    _In_  WDFDEVICE               FxDevice,
    _In_  WDF_POWER_DEVICE_STATE  FxPreviousState
    )
/*++
 
  Routine Description:

    This routine allocates objects needed by the driver.

  Arguments:

    FxDevice - a handle to the framework device object
    FxPreviousState - previous power state

  Return Value:

    Status

--*/
{
    FuncEntry(TRACE_FLAG_WDFLOADING);
    
    UNREFERENCED_PARAMETER(FxPreviousState);

    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
    NTSTATUS status = STATUS_SUCCESS;

	WdfTimerStart(pDevice->Timer, WDF_REL_TIMEOUT_IN_MS(10));

	BOOTTRACKPAD(pDevice);

	pDevice->RegsSet = false;
	pDevice->ConnectInterrupt = true;

    FuncExit(TRACE_FLAG_WDFLOADING);

    return status;
}

NTSTATUS
OnD0Exit(
    _In_  WDFDEVICE               FxDevice,
    _In_  WDF_POWER_DEVICE_STATE  FxPreviousState
    )
/*++
 
  Routine Description:

    This routine destroys objects needed by the driver.

  Arguments:

    FxDevice - a handle to the framework device object
    FxPreviousState - previous power state

  Return Value:

    Status

--*/
{
    FuncEntry(TRACE_FLAG_WDFLOADING);
    
    UNREFERENCED_PARAMETER(FxPreviousState);

    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);

	WdfTimerStop(pDevice->Timer, TRUE);

	pDevice->ConnectInterrupt = false;

    FuncExit(TRACE_FLAG_WDFLOADING);

    return STATUS_SUCCESS;
}

VOID
OnTopLevelIoDefault(
    _In_  WDFQUEUE    FxQueue,
    _In_  WDFREQUEST  FxRequest
    )
/*++

  Routine Description:

    Accepts all incoming requests and pends or forwards appropriately.

  Arguments:

    FxQueue -  Handle to the framework queue object that is associated with the
        I/O request.
    FxRequest - Handle to a framework request object.

  Return Value:

    None.

--*/
{
    FuncEntry(TRACE_FLAG_SPBAPI);
    
    UNREFERENCED_PARAMETER(FxQueue);

    WDFDEVICE device;
    PDEVICE_CONTEXT pDevice;
    WDF_REQUEST_PARAMETERS params;
    NTSTATUS status;

    device = WdfIoQueueGetDevice(FxQueue);
    pDevice = GetDeviceContext(device);

    WDF_REQUEST_PARAMETERS_INIT(&params);

    WdfRequestGetParameters(FxRequest, &params);

	status = WdfRequestForwardToIoQueue(FxRequest, pDevice->SpbQueue);

	if (!NT_SUCCESS(status))
	{
		CyapaPrint(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Failed to forward WDFREQUEST %p to SPB queue %p - %!STATUS!",
			FxRequest,
			pDevice->SpbQueue,
			status);
		
		WdfRequestComplete(FxRequest, status);
	}

    FuncExit(TRACE_FLAG_SPBAPI);
}

VOID
OnIoDeviceControl(
    _In_  WDFQUEUE    FxQueue,
    _In_  WDFREQUEST  FxRequest,
    _In_  size_t      OutputBufferLength,
    _In_  size_t      InputBufferLength,
    _In_  ULONG       IoControlCode
    )
/*++
Routine Description:

    This event is called when the framework receives IRP_MJ_DEVICE_CONTROL
    requests from the system.

Arguments:

    FxQueue - Handle to the framework queue object that is associated
        with the I/O request.
    FxRequest - Handle to a framework request object.
    OutputBufferLength - length of the request's output buffer,
        if an output buffer is available.
    InputBufferLength - length of the request's input buffer,
        if an input buffer is available.
    IoControlCode - the driver-defined or system-defined I/O control code
        (IOCTL) that is associated with the request.

Return Value:

   VOID

--*/
{
    FuncEntry(TRACE_FLAG_SPBAPI);

    WDFDEVICE device;
    PDEVICE_CONTEXT pDevice;
    BOOLEAN fSync = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(FxQueue);
	pDevice = GetDeviceContext(device);

	CyapaPrint(
		DEBUG_LEVEL_INFO, DBG_IOCTL,
        "DeviceIoControl request %p received with IOCTL=%lu",
        FxRequest,
        IoControlCode);
	CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
		"%s, Queue:0x%p, Request:0x%p\n",
		DbgHidInternalIoctlString(IoControlCode),
		FxQueue,
		FxRequest
		);

    //
    // Translate the test IOCTL into the appropriate 
    // SPB API method.  Open and close are completed 
    // synchronously.
    //

    switch (IoControlCode)
    {
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		//
		// Retrieves the device's HID descriptor.
		//
		status = CyapaGetHidDescriptor(device, FxRequest);
		fSync = TRUE;
		break;

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		//
		//Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
		//
		status = CyapaGetDeviceAttributes(FxRequest);
		fSync = TRUE;
		break;

	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		//
		//Obtains the report descriptor for the HID device.
		//
		status = CyapaGetReportDescriptor(device, FxRequest);
		fSync = TRUE;
		break;

	case IOCTL_HID_GET_STRING:
		//
		// Requests that the HID minidriver retrieve a human-readable string
		// for either the manufacturer ID, the product ID, or the serial number
		// from the string descriptor of the device. The minidriver must send
		// a Get String Descriptor request to the device, in order to retrieve
		// the string descriptor, then it must extract the string at the
		// appropriate index from the string descriptor and return it in the
		// output buffer indicated by the IRP. Before sending the Get String
		// Descriptor request, the minidriver must retrieve the appropriate
		// index for the manufacturer ID, the product ID or the serial number
		// from the device extension of a top level collection associated with
		// the device.
		//
		status = CyapaGetString(FxRequest);
		fSync = TRUE;
		break;

	case IOCTL_HID_WRITE_REPORT:
	case IOCTL_HID_SET_OUTPUT_REPORT:
		//
		//Transmits a class driver-supplied report to the device.
		//
		status = CyapaWriteReport(pDevice, FxRequest);
		fSync = TRUE;
		break;

	case IOCTL_HID_READ_REPORT:
	case IOCTL_HID_GET_INPUT_REPORT:
		//
		// Returns a report from the device into a class driver-supplied buffer.
		// 
		status = CyapaReadReport(pDevice, FxRequest, &fSync);
		break;

	case IOCTL_HID_GET_FEATURE:
		//
		// returns a feature report associated with a top-level collection
		//
		status = CyapaGetFeature(pDevice, FxRequest, &fSync);
		break;
	case IOCTL_HID_ACTIVATE_DEVICE:
		//
		// Makes the device ready for I/O operations.
		//
	case IOCTL_HID_DEACTIVATE_DEVICE:
		//
		// Causes the device to cease operations and terminate all outstanding
		// I/O requests.
		//
    default:
        fSync = TRUE;
		status = STATUS_NOT_SUPPORTED;
		CyapaPrint(
			DEBUG_LEVEL_INFO, DBG_IOCTL,
            "Request %p received with unexpected IOCTL=%lu",
            FxRequest,
            IoControlCode);
    }

    //
    // Complete the request if necessary.
    //

    if (fSync)
    {
		CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s completed, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			FxQueue,
			FxRequest
			);

        WdfRequestComplete(FxRequest, status);
	}
	else {
		CyapaPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s deferred, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			FxQueue,
			FxRequest
			);
	}

    FuncExit(TRACE_FLAG_SPBAPI);
}