/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LitePCIe Windows Driver
 *
 * Copyright (C) 2023 / Nate Meyer / Nate.Devel@gmail.com
 *
 */

#include <math.h>

#include "driver.h"
#include "device.tmh"
#include "Trace.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, litepciedrvCreateDevice)
#pragma alloc_text (PAGE, litepciedrvCleanupDevice)
#endif

static UINT32 leftmost_bit(UINT32 x)
{
    UINT32 i = 0;
    while (x > 1)
    {
        x >>= 1;
        i++;
    }
    return i;
}

static NTSTATUS litepciedrv_SetupInterrupts(PDEVICE_CONTEXT dev,
                                            WDFCMRESLIST ResourcesRaw,
                                            WDFCMRESLIST ResourcesTranslated);


UINT32 litepciedrv_RegReadl(PDEVICE_CONTEXT dev, UINT32 reg)
{
    UINT32 val = *(PUINT32)((PUINT8)dev->bar0_addr + reg - CSR_BASE);
    return val;
}

VOID litepciedrv_RegWritel(PDEVICE_CONTEXT dev, UINT32 reg, UINT32 val)
{
    *(PUINT32)((PUINT8)dev->bar0_addr + reg - CSR_BASE) = val;
}

VOID litepciedrvCleanupDevice(
    _In_ WDFOBJECT Object
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Object);
    return;
}

NTSTATUS litepciedrvCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
/*++

Routine Description:

    Worker routine called to create a device and its software resources.

Arguments:

    DeviceInit - Pointer to an opaque init structure. Memory for this
                    structure will be freed by the framework when the WdfDeviceCreate
                    succeeds. So don't access the structure after that point.

Return Value:

    NTSTATUS

--*/
{
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    PDEVICE_CONTEXT deviceContext;
    WDFDEVICE device;
    NTSTATUS status;

    PAGED_CODE();

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.EvtCleanupCallback = litepciedrvCleanupDevice;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);

    if (NT_SUCCESS(status)) {
        //
        // Get a pointer to the device context structure that we just associated
        // with the device object. We define this structure in the device.h
        // header file. DeviceGetContext is an inline function generated by
        // using the WDF_DECLARE_CONTEXT_TYPE_WITH_NAME macro in device.h.
        // This function will do the type checking and return the device context.
        // If you pass a wrong object handle it will return NULL and assert if
        // run under framework verifier mode.
        //
        deviceContext = DeviceGetContext(device);

        //
        // Initialize the context.
        //
        deviceContext->deviceDrv = 0;

        //
        // Create a device interface so that applications can find and talk
        // to us.
        //
        status = WdfDeviceCreateDeviceInterface(
            device,
            &GUID_DEVINTERFACE_litepciedrv,
            NULL // ReferenceString
            );


        if (NT_SUCCESS(status)) {
            //
            // Initialize the I/O Package and any Queues
            //
            status = litepciedrvQueueInitialize(device);
        }
    }

    return status;
}


NTSTATUS litepciedrv_DeviceOpen(WDFDEVICE wdfDevice,
    PDEVICE_CONTEXT litepcie,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated)
{

    NTSTATUS status = STATUS_SUCCESS;

    //Initialize PCI Device Struct
    memset(litepcie, 0x00, sizeof(DEVICE_CONTEXT));
    litepcie->deviceDrv = wdfDevice;

    WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &litepcie->dmaLock);

    //Check Device Version
    //TODO

    //Get BAR0 Config
    const ULONG numRes = WdfCmResourceListGetCount(ResourcesTranslated);
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE, "# PCIe resources = %d", numRes);

    for (UINT8 i = 0; i < numRes; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR resource = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (!resource) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "WdfResourceCmGetDescriptor() fails");
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        if (resource->Type == CmResourceTypeMemory) {
            litepcie->bar0_size = resource->u.Memory.Length;
            litepcie->bar0_addr = MmMapIoSpace(resource->u.Memory.Start,
                resource->u.Memory.Length, MmNonCached);
            if (litepcie->bar0_addr == NULL) {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "MmMapIoSpace returned NULL! for BAR%u", 0);
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DEVICE, "MM BAR %d (addr:0x%lld, length:%u) mapped at 0x%08p",
                0, resource->u.Memory.Start.QuadPart,
                resource->u.Memory.Length, litepcie->bar0_addr);
            break;
        }
    }

    //Reset LitePCIe Core
    litepciedrv_RegWritel(litepcie, CSR_CTRL_RESET_ADDR, 1);

    //Show Identifier
    CHAR versionStr[256] = { 0 };
    for (UINT32 i = 0; i < 256; i++)
    {
        versionStr[i] = (CHAR)litepciedrv_RegReadl(litepcie, CSR_IDENTIFIER_MEM_BASE + i * 4);
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "Version %s", versionStr);

    //TODO: MSI(X) Configuration
    // Only MSI supported for now
    status = litepciedrv_SetupInterrupts(litepcie, ResourcesRaw, ResourcesTranslated);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "Failed to setup interrupts: %!STATUS!", status);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    //Enumerate Userspace Device Interface
    litepcie->channels = DMA_CHANNELS;

    for (UINT32 i = 0; i < litepcie->channels; i++) {
        litepcie->chan[i].index = i;
        litepcie->chan[i].block_size = DMA_BUFFER_SIZE;
        litepcie->chan[i].litepcie_dev = litepcie;
        litepcie->chan[i].dma.writer_lock = 0;
        litepcie->chan[i].dma.reader_lock = 0;

        WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &litepcie->chan[i].dma.readerLock);
        WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &litepcie->chan[i].dma.writerLock);


        switch (i) {
#ifdef CSR_PCIE_DMA7_BASE
        case 7: {
            litepcie->chan[i].dma.base = CSR_PCIE_DMA7_BASE;
            litepcie->chan[i].dma.writer_interrupt = PCIE_DMA7_WRITER_INTERRUPT;
            litepcie->chan[i].dma.reader_interrupt = PCIE_DMA7_READER_INTERRUPT;
        }
              break;
#endif
#ifdef CSR_PCIE_DMA6_BASE
        case 6: {
            litepcie->chan[i].dma.base = CSR_PCIE_DMA6_BASE;
            litepcie->chan[i].dma.writer_interrupt = PCIE_DMA6_WRITER_INTERRUPT;
            litepcie->chan[i].dma.reader_interrupt = PCIE_DMA6_READER_INTERRUPT;
        }
              break;
#endif
#ifdef CSR_PCIE_DMA5_BASE
        case 5: {
            litepcie->chan[i].dma.base = CSR_PCIE_DMA5_BASE;
            litepcie->chan[i].dma.writer_interrupt = PCIE_DMA5_WRITER_INTERRUPT;
            litepcie->chan[i].dma.reader_interrupt = PCIE_DMA5_READER_INTERRUPT;
        }
              break;
#endif
#ifdef CSR_PCIE_DMA4_BASE
        case 4: {
            litepcie->chan[i].dma.base = CSR_PCIE_DMA4_BASE;
            litepcie->chan[i].dma.writer_interrupt = PCIE_DMA4_WRITER_INTERRUPT;
            litepcie->chan[i].dma.reader_interrupt = PCIE_DMA4_READER_INTERRUPT;
        }
              break;
#endif
#ifdef CSR_PCIE_DMA3_BASE
        case 3: {
            litepcie->chan[i].dma.base = CSR_PCIE_DMA3_BASE;
            litepcie->chan[i].dma.writer_interrupt = PCIE_DMA3_WRITER_INTERRUPT;
            litepcie->chan[i].dma.reader_interrupt = PCIE_DMA3_READER_INTERRUPT;
        }
              break;
#endif
#ifdef CSR_PCIE_DMA2_BASE
        case 2: {
            litepcie->chan[i].dma.base = CSR_PCIE_DMA2_BASE;
            litepcie->chan[i].dma.writer_interrupt = PCIE_DMA2_WRITER_INTERRUPT;
            litepcie->chan[i].dma.reader_interrupt = PCIE_DMA2_READER_INTERRUPT;
        }
              break;
#endif
#ifdef CSR_PCIE_DMA1_BASE
        case 1: {
            litepcie->chan[i].dma.base = CSR_PCIE_DMA1_BASE;
            litepcie->chan[i].dma.writer_interrupt = PCIE_DMA1_WRITER_INTERRUPT;
            litepcie->chan[i].dma.reader_interrupt = PCIE_DMA1_READER_INTERRUPT;
        }
              break;
#endif
        default:
        {
            litepcie->chan[i].dma.base = CSR_PCIE_DMA0_BASE;
            litepcie->chan[i].dma.writer_interrupt = PCIE_DMA0_WRITER_INTERRUPT;
            litepcie->chan[i].dma.reader_interrupt = PCIE_DMA0_READER_INTERRUPT;
        }
        break;
        }
    }

    //Create DMA Enabler
    WdfDeviceSetAlignmentRequirement(litepcie->deviceDrv, FILE_LONG_ALIGNMENT);

    WDF_DMA_ENABLER_CONFIG dmaConfig;
    WDF_DMA_ENABLER_CONFIG_INIT(&dmaConfig, WdfDmaProfileScatterGather64Duplex, DMA_BUFFER_SIZE);
    status = WdfDmaEnablerCreate(litepcie->deviceDrv, &dmaConfig, WDF_NO_OBJECT_ATTRIBUTES, &litepcie->dmaEnabler);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Failed to create dmaEnabler: %!STATUS!", status);
        return status;
    }

    status = WdfDmaTransactionCreate(litepcie->dmaEnabler, WDF_NO_OBJECT_ATTRIBUTES, &litepcie->dmaTransaction);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Failed to create dmaTransaction: %!STATUS!", status);
        return status;
    }

    //Allocate DMA Buffers
    /* for each dma channel */
    for (UINT32 i = 0; i < litepcie->channels; i++) {
        struct litepcie_dma_chan* dmachan = &litepcie->chan[i].dma;
        //Allocate Common buffer for the channel read transactions
        status = WdfCommonBufferCreate(litepcie->dmaEnabler,
                                        DMA_BUFFER_TOTAL_SIZE,
                                        WDF_NO_OBJECT_ATTRIBUTES,
                                        &dmachan->readBuffer);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Failed to create Read Buffer for channel %d: %!STATUS!", i, status);
            return status;
        }

        //Allocate a Common buffer for the channel write transactions
        status = WdfCommonBufferCreate(litepcie->dmaEnabler,
                                        DMA_BUFFER_TOTAL_SIZE,
                                        WDF_NO_OBJECT_ATTRIBUTES,
                                        &dmachan->writeBuffer);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Failed to create Write Buffer for channel %d: %!STATUS!", i, status);
            return status;
        }
        
        PVOID rdVirtAddr = WdfCommonBufferGetAlignedVirtualAddress(dmachan->readBuffer);
        PHYSICAL_ADDRESS rdPhysAddr = WdfCommonBufferGetAlignedLogicalAddress(dmachan->readBuffer);
        PVOID wrVirtAddr = WdfCommonBufferGetAlignedVirtualAddress(dmachan->writeBuffer);
        PHYSICAL_ADDRESS wrPhysAddr = WdfCommonBufferGetAlignedLogicalAddress(dmachan->writeBuffer);

        /* for each dma buffer */
        for (UINT32 j = 0; j < DMA_BUFFER_COUNT; j++) {
            /* assign rd mdl */
            dmachan->reader_handle[j] = (PVOID)((PUINT8)rdVirtAddr + (j * DMA_BUFFER_SIZE));
            dmachan->reader_addr[j].QuadPart = rdPhysAddr.QuadPart + (j * DMA_BUFFER_SIZE);
            /* assign wr mdl */
            dmachan->writer_handle[j] = (PVOID)((PUINT8)wrVirtAddr + (j * DMA_BUFFER_SIZE));
            dmachan->writer_addr[j].QuadPart = wrPhysAddr.QuadPart + (j * DMA_BUFFER_SIZE);

            /* check */
            if ((dmachan->reader_addr[j].QuadPart == 0)
                || (dmachan->writer_addr[j].QuadPart == 0)) {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Failed to allocate dma buffer for index %d\n", i);
                return STATUS_NO_MEMORY;
            }
        }
    }

    return status;
}


NTSTATUS litepciedrv_DeviceClose(WDFDEVICE wdfDevice)
{
    UNREFERENCED_PARAMETER(wdfDevice);
    PDEVICE_CONTEXT litepcie = DeviceGetContext(wdfDevice);

    /* Stop the DMAs */
    for (UINT32 i = 0; i < litepcie->channels; i++) {
        struct litepcie_dma_chan *dmachan = &litepcie->chan[i].dma;
        litepciedrv_RegWritel(litepcie, dmachan->base + PCIE_DMA_WRITER_ENABLE_OFFSET, 0);
        litepciedrv_RegWritel(litepcie, dmachan->base + PCIE_DMA_READER_ENABLE_OFFSET, 0);
    }

    /* Disable all interrupts */
    litepciedrv_RegWritel(litepcie, CSR_PCIE_MSI_ENABLE_ADDR, 0);

    MmUnmapIoSpace(litepcie->bar0_addr, litepcie->bar0_size);

    /* Remove Userspace Device Interfaces*/

    return STATUS_SUCCESS;
}


VOID litepciedrv_ChannelRead(PLITEPCIE_CHAN channel, WDFREQUEST request, SIZE_T length)
{
    SIZE_T bytesRead = 0;
    UINT32 overflows = 0;
    WDFMEMORY outBuf;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputMemory(request, &outBuf);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "WdfRequestRetrieveOutputMemory failed %x\n", status);
        WdfRequestCompleteWithInformation(request, status, 0);
        return;
    }
    
    while (bytesRead < length)
    {
        if ((length - bytesRead) < DMA_BUFFER_SIZE)
        {
            //Only allow reading in increments of DMA_BUFFER_SIZE
            break;
        }

        // Get available buffers
        // LITEPCIE DMA calls C2H channel the "writer"
        WdfSpinLockAcquire(channel->dma.writerLock);
        INT64 available_count = channel->dma.writer_hw_count - channel->dma.writer_sw_count;
        WdfSpinLockRelease(channel->dma.writerLock);

        if ((available_count) > 0)
        {
            if ((available_count) > (DMA_BUFFER_COUNT - DMA_BUFFER_PER_IRQ))
            {
                overflows++;
            }

            WdfMemoryCopyFromBuffer(outBuf, bytesRead,
                channel->dma.writer_handle[channel->dma.writer_sw_count % DMA_BUFFER_COUNT],
                DMA_BUFFER_SIZE);
            channel->dma.writer_sw_count += 1;
            bytesRead += DMA_BUFFER_SIZE;
        }
        else
        {
            //Defer for more data
            break;
        }
    }

    if (overflows > 0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Overflow Error in ChannelRead: %d\n", overflows);
    }

    //if ((length - bytesRead) < DMA_BUFFER_SIZE)
    if (bytesRead > 0)
    {
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, bytesRead);
        channel->dma.readRequest = NULL;
        channel->dma.readRemainingBytes = 0;
    }
    else
    {
        channel->dma.readRequest = request;
        channel->dma.readRemainingBytes = length - bytesRead;
    }
}

VOID litepciedrv_ChannelWrite(PLITEPCIE_CHAN channel, WDFREQUEST request, SIZE_T length)
{
    SIZE_T bytesWritten = 0;
    UINT32 overflows = 0;
    WDFMEMORY inBuf;
    NTSTATUS status;

    status = WdfRequestRetrieveInputMemory(request, &inBuf);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "WdfRequestRetrieveInputMemory failed %x\n", status);
        WdfRequestCompleteWithInformation(request, status, 0);
        return;
    }
    while (bytesWritten < length)
    {
        if ((length - bytesWritten) < DMA_BUFFER_SIZE)
        {
            //Only allow writing in increments of DMA_BUFFER_SIZE
            break;
        }

        // Get available buffers
        // LITEPCIE DMA calls H2C channel the "reader"
        WdfSpinLockAcquire(channel->dma.readerLock);
        INT64 available_count = channel->dma.reader_hw_count - channel->dma.reader_sw_count;
        WdfSpinLockRelease(channel->dma.readerLock);

        if ((available_count) > 0)
        {
            if ((available_count) > DMA_BUFFER_COUNT - DMA_BUFFER_PER_IRQ)
            {
                overflows++;
            }

            WdfMemoryCopyToBuffer(inBuf, bytesWritten,
                channel->dma.reader_handle[channel->dma.reader_sw_count % DMA_BUFFER_COUNT],
                DMA_BUFFER_SIZE);
            channel->dma.reader_sw_count += 1;
            bytesWritten += DMA_BUFFER_SIZE;
        }
        else
        {
            break;
        }
    }

    if (overflows > 0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Overflow Error in ChannelWrite: %d\n", overflows);
    }

    //if ((length - bytesWritten) < DMA_BUFFER_SIZE)
    if (bytesWritten > 0)
    {
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, bytesWritten);
        channel->dma.writeRequest = NULL;
        channel->dma.writeRemainingBytes = 0;
    }
    else
    {
        channel->dma.writeRequest = request;
        channel->dma.writeRemainingBytes = length - bytesWritten;
    }
}

VOID litepcie_dma_writer_start(PDEVICE_CONTEXT dev, UINT32 index)
{
    struct litepcie_dma_chan* dmachan;
    UINT32 i;

    dmachan = &dev->chan[index].dma;

    /* Fill DMA Writer descriptors. */
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_ENABLE_OFFSET, 0);
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_TABLE_FLUSH_OFFSET, 1);
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_TABLE_LOOP_PROG_N_OFFSET, 0);
    for (i = 0; i < DMA_BUFFER_COUNT; i++)
    {
        /* Fill buffer size + parameters. */
        litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_TABLE_VALUE_OFFSET,
#ifndef DMA_BUFFER_ALIGNED
            DMA_LAST_DISABLE |
#endif
            (!(i % DMA_BUFFER_PER_IRQ == 0)) * DMA_IRQ_DISABLE | /* generate an msi */
            DMA_BUFFER_SIZE);                                  /* every n buffers */
        /* Get Phys Address from MDL */
        //MmProbeAndLockPages(dmachan->writerList[i], KernelMode, IoWriteAccess);
        //PVOID physAddr = MmGetSystemAddressForMdlSafe(dmachan->writerList[i], NormalPagePriority);
        /* Fill 32-bit Address LSB. */
        litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_TABLE_VALUE_OFFSET + 4, dmachan->writer_addr[index].LowPart);
        /* Write descriptor (and fill 32-bit Address MSB for 64-bit mode). */
        litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_TABLE_WE_OFFSET, dmachan->writer_addr[index].HighPart);
    }
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_TABLE_LOOP_PROG_N_OFFSET, 1);

    /* Clear counters. */
    dmachan->writer_hw_count = 0;
    dmachan->writer_hw_count_last = 0;
    dmachan->writer_sw_count = 0;

    /* Start DMA Writer. */
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_ENABLE_OFFSET, 1);
}

VOID litepcie_dma_writer_stop(PDEVICE_CONTEXT dev, UINT32 index)
{
    struct litepcie_dma_chan* dmachan;

    dmachan = &dev->chan[index].dma;

    /* Flush and stop DMA Writer. */
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_TABLE_LOOP_PROG_N_OFFSET, 0);
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_TABLE_FLUSH_OFFSET, 1);
    KeStallExecutionProcessor(1000);
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_ENABLE_OFFSET, 0);
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_WRITER_TABLE_FLUSH_OFFSET, 1);

    //Unlock buffer pages
    //for (UINT32 i = 0; i < DMA_BUFFER_COUNT; i++)
    //{
    //    MmUnlockPages(dmachan->writerList[i]);
    //}

    /* Clear counters. */
    dmachan->writer_hw_count = 0;
    dmachan->writer_hw_count_last = 0;
    dmachan->writer_sw_count = 0;
}

VOID litepcie_dma_reader_start(PDEVICE_CONTEXT dev, UINT32 index)
{
    struct litepcie_dma_chan* dmachan;
    UINT32 i;

    dmachan = &dev->chan[index].dma;

    /* Fill DMA Reader descriptors. */
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_ENABLE_OFFSET, 0);
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_TABLE_FLUSH_OFFSET, 1);
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_TABLE_LOOP_PROG_N_OFFSET, 0);
    for (i = 0; i < DMA_BUFFER_COUNT; i++)
    {
        /* Fill buffer size + parameters. */
        litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_TABLE_VALUE_OFFSET,
#ifndef DMA_BUFFER_ALIGNED
            DMA_LAST_DISABLE |
#endif
            (!(i % DMA_BUFFER_PER_IRQ == 0)) * DMA_IRQ_DISABLE | /* generate an msi */
            DMA_BUFFER_SIZE);                                  /* every n buffers */
        /* Get Phys Address from MDL */
        //MmProbeAndLockPages(dmachan->readerList[i], KernelMode, IoWriteAccess);
        //PVOID physAddr = MmGetSystemAddressForMdlSafe(dmachan->readerList[i], NormalPagePriority);
        /* Fill 32-bit Address LSB. */
        litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_TABLE_VALUE_OFFSET + 4, dmachan->writer_addr[index].LowPart);
        /* Write descriptor (and fill 32-bit Address MSB for 64-bit mode). */
        litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_TABLE_WE_OFFSET, dmachan->writer_addr[index].HighPart);
    }
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_TABLE_LOOP_PROG_N_OFFSET, 1);

    /* clear counters */
    dmachan->reader_hw_count = 0;
    dmachan->reader_hw_count_last = 0;
    dmachan->reader_sw_count = 0;

    /* start dma reader */
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_ENABLE_OFFSET, 1);
}

VOID litepcie_dma_reader_stop(PDEVICE_CONTEXT dev, UINT32 index)
{
    struct litepcie_dma_chan* dmachan;

    dmachan = &dev->chan[index].dma;

    /* Flush and stop DMA Writer. */
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_TABLE_LOOP_PROG_N_OFFSET, 0);
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_TABLE_FLUSH_OFFSET, 1);
    KeStallExecutionProcessor(1000);
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_ENABLE_OFFSET, 0);
    litepciedrv_RegWritel(dev, dmachan->base + PCIE_DMA_READER_TABLE_FLUSH_OFFSET, 1);

    //Unlock buffer pages
    //for (UINT32 i = 0; i < DMA_BUFFER_COUNT; i++)
    //{
    //  MmUnlockPages(dmachan->readerList[i]);
    //}

    /* Clear counters. */
    dmachan->reader_hw_count = 0;
    dmachan->reader_hw_count_last = 0;
    dmachan->reader_sw_count = 0;
}

VOID litepcie_enable_interrupt(PDEVICE_CONTEXT dev, UINT32 interrupt)
{
    dev->irqs_requested |= (1 << interrupt);

    litepciedrv_RegWritel(dev, CSR_PCIE_MSI_ENABLE_ADDR, dev->irqs_requested);
    litepciedrv_RegWritel(dev, CSR_PCIE_MSI_CLEAR_ADDR, (1 << interrupt));
}

VOID litepcie_disable_interrupt(PDEVICE_CONTEXT dev, UINT32 interrupt)
{
    dev->irqs_requested &= ~(1 << interrupt);

    litepciedrv_RegWritel(dev, CSR_PCIE_MSI_ENABLE_ADDR, dev->irqs_requested);
}

NTSTATUS litepcie_EvtIntEnable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(AssociatedDevice);
    UNREFERENCED_PARAMETER(Interrupt);
    
    litepciedrv_RegWritel(ctx, CSR_PCIE_MSI_ENABLE_ADDR, ctx->irqs_requested);
    litepciedrv_RegWritel(ctx, CSR_PCIE_MSI_CLEAR_ADDR, ctx->irqs_requested);


    return STATUS_SUCCESS;
}

NTSTATUS litepcie_EvtIntDisable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(AssociatedDevice);
    UNREFERENCED_PARAMETER(Interrupt);

    litepciedrv_RegWritel(ctx, CSR_PCIE_MSI_ENABLE_ADDR, 0);

    return STATUS_SUCCESS;
}


BOOLEAN litepcie_EvtIsr(IN WDFINTERRUPT Interrupt, IN ULONG MessageID)
{
    UNREFERENCED_PARAMETER(MessageID);
    PDEVICE_CONTEXT dev = DeviceGetContext(WdfInterruptGetDevice(Interrupt));
    UINT32 irqVec = litepciedrv_RegReadl(dev, CSR_PCIE_MSI_VECTOR_ADDR);

    if (irqVec != 0)
    {
        dev->irqs_pending |= irqVec;
#ifdef CSR_PCIE_MSI_CLEAR_ADDR
        litepciedrv_RegWritel(dev, CSR_PCIE_MSI_CLEAR_ADDR, irqVec);
#endif
        WdfInterruptQueueDpcForIsr(Interrupt);
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

VOID litepcie_EvtDpc(IN WDFINTERRUPT Interrupt, IN WDFOBJECT device)
{
    UNREFERENCED_PARAMETER(device);
    UINT32 irq_vector, irq_enable, clear_mask = 0;
    PDEVICE_CONTEXT dev = DeviceGetContext(WdfInterruptGetDevice(Interrupt));
    PLITEPCIE_CHAN pChan;
    UINT32 loop_status, i;

    irq_enable = litepciedrv_RegReadl(dev, CSR_PCIE_MSI_ENABLE_ADDR);
    irq_vector = dev->irqs_pending & irq_enable;

    for (i = 0; i < dev->channels; i++) {
        pChan = &dev->chan[i];
        /* dma reader interrupt handling */
        if (irq_vector & (1 << pChan->dma.reader_interrupt)) {
            loop_status = litepciedrv_RegReadl(dev, pChan->dma.base +
                PCIE_DMA_READER_TABLE_LOOP_STATUS_OFFSET);
            WdfSpinLockAcquire(pChan->dma.readerLock);
            pChan->dma.reader_hw_count &= ((~(DMA_BUFFER_COUNT - 1) << 16) & 0xffffffffffff0000);
            pChan->dma.reader_hw_count |= (loop_status >> 16) * DMA_BUFFER_COUNT + (loop_status & 0xffff);
            if (pChan->dma.reader_hw_count_last > pChan->dma.reader_hw_count)
                pChan->dma.reader_hw_count += (INT64)(1UL << (leftmost_bit(DMA_BUFFER_COUNT) + 16));
            pChan->dma.reader_hw_count_last = pChan->dma.reader_hw_count;
            WdfSpinLockRelease(pChan->dma.readerLock);
#ifdef DEBUG_MSI
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "MSI DMA%d Reader buf: %lld\n", i,
                pChan->dma.reader_hw_count);
#endif
            if (pChan->dma.writeRequest != NULL)
            {
                litepciedrv_ChannelWrite(pChan, pChan->dma.writeRequest, pChan->dma.writeRemainingBytes);
            }
            clear_mask |= (1 << pChan->dma.reader_interrupt);
        }
        /* dma writer interrupt handling */
        if (irq_vector & (1 << pChan->dma.writer_interrupt)) {
            loop_status = litepciedrv_RegReadl(dev, pChan->dma.base +
                PCIE_DMA_WRITER_TABLE_LOOP_STATUS_OFFSET);
            WdfSpinLockAcquire(pChan->dma.writerLock);
            pChan->dma.writer_hw_count &= ((~(DMA_BUFFER_COUNT - 1) << 16) & 0xffffffffffff0000);
            pChan->dma.writer_hw_count |= (loop_status >> 16) * DMA_BUFFER_COUNT + (loop_status & 0xffff);
            if (pChan->dma.writer_hw_count_last > pChan->dma.writer_hw_count)
                pChan->dma.writer_hw_count += (INT64)(1UL << (leftmost_bit(DMA_BUFFER_COUNT) + 16));
            pChan->dma.writer_hw_count_last = pChan->dma.writer_hw_count;
            WdfSpinLockRelease(pChan->dma.writerLock);
#ifdef DEBUG_MSI
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "MSI DMA%d Writer buf: %lld\n", i,
                pChan->dma.writer_hw_count);
#endif
            if (pChan->dma.readRequest != NULL)
            {
                litepciedrv_ChannelRead(pChan, pChan->dma.readRequest, pChan->dma.readRemainingBytes);
            }
            clear_mask |= (1 << pChan->dma.writer_interrupt);
        }
    }
    dev->irqs_pending &= ~clear_mask;
}

static NTSTATUS litepciedrv_SetupInterrupts(PDEVICE_CONTEXT dev,
                                            WDFCMRESLIST ResourcesRaw,
                                            WDFCMRESLIST ResourcesTranslated)
{
    NTSTATUS status = STATUS_SUCCESS;
    UINT32 irqs = 0;

    dev->irqs_requested = 0;

    for (UINT32 i = 0; i < WdfCmResourceListGetCount(ResourcesTranslated); i++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc->Type == CmResourceTypeInterrupt)
        {
            irqs++;
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%d MSI IRQs allocated.\n", irqs);
    for (UINT32 i = 0; i < WdfCmResourceListGetCount(ResourcesTranslated); i++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i); 
        if (desc->Type == CmResourceTypeInterrupt)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "Creating interrupt for MSI %ul.\n", desc->u.MessageInterrupt.Translated.Vector);

            WDF_INTERRUPT_CONFIG config;
            WDF_INTERRUPT_CONFIG_INIT(&config, litepcie_EvtIsr, litepcie_EvtDpc);
            config.InterruptRaw = WdfCmResourceListGetDescriptor(ResourcesRaw, i);
            config.InterruptTranslated = desc;
            config.EvtInterruptEnable = litepcie_EvtIntEnable;
            config.EvtInterruptDisable = litepcie_EvtIntDisable;

            status = WdfInterruptCreate(dev->deviceDrv, &config, WDF_NO_OBJECT_ATTRIBUTES,
                &(dev->intr));

            //Setup Interrupt context
            WDF_INTERRUPT_INFO intInfo;
            WDF_INTERRUPT_INFO_INIT(&intInfo);
            WdfInterruptGetInfo(dev->intr, &intInfo);
            
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "Registered Interrupt."
                    "Vector: %u MessageSignaled: %u MessageNo: %u\n", intInfo.Vector, intInfo.MessageSignaled, intInfo.MessageNumber);

            break;

        }
    }
    return status;
}