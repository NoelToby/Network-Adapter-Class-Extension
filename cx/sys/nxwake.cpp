// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Abstract:

    This implements the NETPOWERSETTINGS object.

--*/

#include "Nx.hpp"

#include "NxWake.tmh"
#include "NxWake.hpp"

#include "NxAdapter.hpp"
#include "NxDevice.hpp"
#include "NxMacros.hpp"
#include "version.hpp"

NxWake::NxWake(
    _In_ NETPOWERSETTINGS    NetPowerSettingsWdfHandle,
    _In_ NxAdapter *         NxAdapter) :
    CFxObject(NetPowerSettingsWdfHandle),
    m_NxAdapter(NxAdapter),
    m_WakeListCount(0),
    m_ProtocolOffloadListCount(0)
{
    m_WakeListHead.Next = NULL;
    m_ProtocolOffloadListHead.Next = NULL;
    m_DriverObjectCallbacksEnabled = FALSE;
    m_EvtCleanupCallback = NULL;
    m_EvtDestroyCallback = NULL;
    m_DriverCanAccessWakeSettings = FALSE;

    RtlZeroMemory(&m_PMParameters, sizeof(m_PMParameters));
}

NxWake::~NxWake(
    VOID
    )
{
    PNX_NET_POWER_ENTRY nxPowerEntry;
    PSINGLE_LIST_ENTRY listEntry;

    while (m_WakeListHead.Next != NULL) {
        listEntry = PopEntryList(&m_WakeListHead);
        nxPowerEntry = CONTAINING_RECORD(listEntry, NX_NET_POWER_ENTRY, ListEntry);
        ExFreePoolWithTag(nxPowerEntry, NETADAPTERCX_TAG);
    }

    while (m_ProtocolOffloadListHead.Next != NULL) {
        listEntry = PopEntryList(&m_ProtocolOffloadListHead);
        nxPowerEntry = CONTAINING_RECORD(listEntry, NX_NET_POWER_ENTRY, ListEntry);
        ExFreePoolWithTag(nxPowerEntry, NETADAPTERCX_TAG);
    }
}

VOID
NxWake::_EvtCleanupCallbackWrapper(
    _In_ WDFOBJECT Object
)
/*++
Routine Description:
    Wraps the driver supplied callback so we can disable them until
    adapter initialization is complete.

Arguments:
    WDFOBJECT being disposed.

Returns:
    VOID
--*/
{
    auto pNxWake = GetNxWakeFromHandle((NETPOWERSETTINGS)Object);

    if (pNxWake->m_DriverObjectCallbacksEnabled) {
        if (pNxWake->m_EvtCleanupCallback != NULL) {
            return pNxWake->m_EvtCleanupCallback(Object);
        }
    }
}

VOID
NxWake::_EvtDestroyCallbackWrapper(
    _In_ WDFOBJECT Object
)
/*++
Routine Description:
    See _EvtCleanupCallbackWrapper

Arguments:
    WDFOBJECT being destoryed.

Returns:
    VOID
--*/
{
    auto pNxWake = GetNxWakeFromHandle((NETPOWERSETTINGS)Object);

    if (pNxWake->m_DriverObjectCallbacksEnabled) {
        if (pNxWake->m_EvtDestroyCallback != NULL) {
            return pNxWake->m_EvtDestroyCallback(Object);
        }
    }
}

NTSTATUS
NxWake::_Create(
    _In_  NxAdapter *              NetAdapter,
    _In_  PWDF_OBJECT_ATTRIBUTES   NetPowerSettingsObjectAttributes,
    _Out_ NxWake **                NxWakeObj
)
/*++
Routine Description:
    Static method that creates the NETWAKE object.

Arguments:
    NxAdapter - Pointer to the NxAdapter object associated with the wake object
    NxWake ** - Out parameter to the allocated wake object if sucessful

Returns:
    NTSTATUS

--*/
{
    NTSTATUS                status;
    WDF_OBJECT_ATTRIBUTES   attributes;
    NETPOWERSETTINGS        netPowerSettingsWdfObj;
    PWDF_OBJECT_ATTRIBUTES  driversNetPowerObjAttributes;

    //
    // Create a WDFOBJECT for NETPOWERSETTINGS
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NxWake);
    attributes.ParentObject = NetAdapter->GetFxObject();

    //
    // Ensures that the destructor is called when this object is destroyed.
    //
    NxWake::_SetObjectAttributes(&attributes);

    status = WdfObjectCreate(&attributes, (WDFOBJECT*)&netPowerSettingsWdfObj);
    if (!NT_SUCCESS(status)) {
        LogError(NetAdapter->GetRecorderLog(), FLAG_POWER,
                 "WdfObjectCreate for NETPOWERSETTINGS failed %!STATUS!", status);
        return status;
    }

    //
    // Get the NxWake allocation, which is the ctx associated with the
    // WDF object.
    //
    void * netPowerSettingsWdfObjCtx = GetNxWakeFromHandle(netPowerSettingsWdfObj);

    //
    // Use the inplacement new and invoke the NxWake constructor
    //
    auto pNxWake = new (netPowerSettingsWdfObjCtx) NxWake(netPowerSettingsWdfObj,
                                                NetAdapter);
    __analysis_assume(pNxWake != NULL);
    NT_ASSERT(pNxWake);

    if (NetPowerSettingsObjectAttributes->Size != 0) {
        //
        // NxWake is created as part of NxAdapter creation. In case something
        // fails after this returns, we dont want the driver's object cleanup
        // or destroy callbacks to be invoked. So we swap the callbacks with
        // our wrapper and only enable these driver callbacks after adapter
        // initialization has completed successfully
        //
        driversNetPowerObjAttributes = NetPowerSettingsObjectAttributes;

        pNxWake->m_EvtCleanupCallback = driversNetPowerObjAttributes->EvtCleanupCallback;
        pNxWake->m_EvtDestroyCallback = driversNetPowerObjAttributes->EvtDestroyCallback;

        driversNetPowerObjAttributes->EvtCleanupCallback = NxWake::_EvtCleanupCallbackWrapper;
        driversNetPowerObjAttributes->EvtDestroyCallback = NxWake::_EvtDestroyCallbackWrapper;

        status = WdfObjectAllocateContext(netPowerSettingsWdfObj,
                                        driversNetPowerObjAttributes,
                                        NULL);
        if (!NT_SUCCESS(status)) {
            LogError(NetAdapter->GetRecorderLog(), FLAG_POWER,
                "WdfObjectAllocateContext with NetPowerSettingsObjectAttributes failed %!STATUS!", status);
            WdfObjectDelete(netPowerSettingsWdfObj);
            return status;
        }
    }

    //
    // The client's object attributes have been associated with the
    // wake object already. Dont introduce failures after this point.
    //
    *NxWakeObj = pNxWake;

    return status;
}

NDIS_STATUS
NxWake::AddProtocolOffload(
    _In_ NETADAPTER AdapterWdfHandle,
    _In_ NDIS_OID_REQUEST::_REQUEST_DATA::_SET const *SetInformation
    )
/*++
Routine Description:
    Processes addition of protocol offloads. The added offload is provided
    to the driver for filtering if it has registered a preview callback to
    process it.

Arguments:
    AdapterWdfHandle - NETADAPTER handle
    SetInformation - SET_INFORMATION parameter from the incoming OID

Returns:
    NDIS_STATUS
    Note if driver returns status other than  NDIS_STATUS_PM_PROTOCOL_OFFLOAD_LIST_FULL
    from m_EvtPreviewProtocolOffload it is treated as NDIS_STATUS_SUCCESS.

--*/
{
    ASSERT(! m_DriverCanAccessWakeSettings);

    PNX_NET_POWER_ENTRY nxPowerEntry;
    NTSTATUS status = STATUS_SUCCESS;
    PNDIS_PM_PROTOCOL_OFFLOAD ndisProtocolOffload;

    ndisProtocolOffload = (PNDIS_PM_PROTOCOL_OFFLOAD)SetInformation->InformationBuffer;

    //
    // Allocate and make a copy before invoking the callback to avoid
    // failure after driver accepts a pattern
    //
    nxPowerEntry = CreateProtocolOffloadEntry(
        ndisProtocolOffload,
        SetInformation->InformationBufferLength);
    if (nxPowerEntry == NULL)
    {
        return NdisConvertNtStatusToNdisStatus(STATUS_INSUFFICIENT_RESOURCES);
    }

    //
    // Change nxWakeEntry to see if it is enabled.
    //
    UpdateProtocolOffloadEntryEnabledField(nxPowerEntry);

    //
    // Invoke optional callback
    //
    if (m_EvtPreviewProtocolOffload != NULL)
    {
        m_DriverCanAccessWakeSettings = TRUE;
        status = m_EvtPreviewProtocolOffload(AdapterWdfHandle,
            GetFxObject(),
            nxPowerEntry->NdisProtocolOffload.ProtocolOffloadType,
            &(nxPowerEntry->NdisProtocolOffload));
        m_DriverCanAccessWakeSettings = FALSE;

        if (status == STATUS_NDIS_PM_PROTOCOL_OFFLOAD_LIST_FULL)
        {
            //
            // In case driver incorrectly saved a pointer to the pattern
            // help catch it sooner.
            //
            RtlFillMemory(nxPowerEntry, nxPowerEntry->Size, 0xC0);
            ExFreePoolWithTag(nxPowerEntry, NETADAPTERCX_TAG);
            return NDIS_STATUS_PM_PROTOCOL_OFFLOAD_LIST_FULL;
        }
    }

    //
    // Add it to the list
    //
    m_ProtocolOffloadListCount++;
    PushEntryList(&m_ProtocolOffloadListHead, &nxPowerEntry->ListEntry);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NxWake::RemoveProtocolOffload(
    _In_ NDIS_OID_REQUEST::_REQUEST_DATA::_SET const *SetInformation
    )
/*++
Routine Description:
    Processes removal of protocol offloads.

Arguments:
    SetInformation - SET_INFORMATION parameter from the incoming OID

Returns:
    NDIS_STATUS

--*/
{
    ASSERT(! m_DriverCanAccessWakeSettings);

    PNX_NET_POWER_ENTRY nxPowerEntry = RemovePowerEntryByID(
        *((PULONG)SetInformation->InformationBuffer),
        NxPowerEntryTypeProtocolOffload);

    RtlFillMemory(nxPowerEntry, nxPowerEntry->Size, 0xC0);
    ExFreePoolWithTag(nxPowerEntry, NETADAPTERCX_TAG);

    return NDIS_STATUS_SUCCESS;
}

RECORDER_LOG
NxWake::GetRecorderLog(
    void
    )
{
    return m_NxAdapter->GetRecorderLog();
}

PNX_NET_POWER_ENTRY
NxWake::CreateProtocolOffloadEntry(
    _In_ PNDIS_PM_PROTOCOL_OFFLOAD  NdisProtocolOffload,
    _In_ UINT                       InformationBufferLength
    )
/*++
Routine Description:
    Create a Nx power entry for protocol offload. Caller must free the
    allocated entry when appropriate.

Arguments:
    Pattern - Incoming PNDIS_PM_PROTOCOL_OFFLOAD
    InformationBufferLength - Length of the buffer containing the offload.

Returns:
    Pointer to an allocated NX_NET_POWER_ENTRY or NULL in case of failure
--*/
{
    NTSTATUS status;
    PNX_NET_POWER_ENTRY pNxPowerEntry;
    ULONG ndisProtocolOffloadSize;
    ULONG totalAllocationSize;

    ndisProtocolOffloadSize = sizeof(NDIS_PM_PROTOCOL_OFFLOAD);

    if (InformationBufferLength < ndisProtocolOffloadSize) {
        LogError(GetRecorderLog(), FLAG_POWER,
            "Invalid InformationBufferLength %u for protocol offload entry", InformationBufferLength);
        return NULL;
    }

   status = RtlULongAdd(sizeof(NX_NET_POWER_ENTRY),
                    ndisProtocolOffloadSize,
                    &totalAllocationSize);
    if (!NT_SUCCESS(status)) {
        LogError(GetRecorderLog(), FLAG_POWER,
            "Unable to compute size requirement for protocol offload entry %!STATUS!", status);
        return NULL;
    }

    pNxPowerEntry = (PNX_NET_POWER_ENTRY)ExAllocatePoolWithTag(NonPagedPoolNx,
                                                            totalAllocationSize,
                                                            NETADAPTERCX_TAG);
    if (pNxPowerEntry == NULL) {
        LogError(GetRecorderLog(), FLAG_POWER, "Allocation for Nx power entry failed");
        return NULL;
    }

    RtlZeroMemory(pNxPowerEntry, totalAllocationSize);
    pNxPowerEntry->Size = totalAllocationSize;

    RtlCopyMemory(&pNxPowerEntry->NdisProtocolOffload,
                NdisProtocolOffload,
                ndisProtocolOffloadSize);

    return pNxPowerEntry;
}

PNX_NET_POWER_ENTRY
NxWake::CreateWakePatternEntry(
    _In_ PNDIS_PM_WOL_PATTERN Pattern,
    _In_ UINT                 InformationBufferLength
)
/*++
Routine Description:
    Create a Nx power pattern entry after taking into account pattern size
    requirements. Caller must free the allocated entry when appropriate.

Arguments:
    Pattern - Incoming PNDIS_PM_WOL_PATTERN
    InformationBufferLength - Length of the buffer containing the Pattern.

Returns:
    Pointer to an allocated NX_NET_POWER_ENTRY or NULL in case of failure
--*/
{
    ULONG patternPayloadSize = 0;
    ULONG totalAllocationSize;
    NTSTATUS status;
    PNX_NET_POWER_ENTRY pNxWakeEntry;

    if (Pattern->WoLPacketType == NdisPMWoLPacketBitmapPattern) {
        patternPayloadSize =
            max(Pattern->WoLPattern.WoLBitMapPattern.MaskOffset + Pattern->WoLPattern.WoLBitMapPattern.MaskSize,
                Pattern->WoLPattern.WoLBitMapPattern.PatternOffset + Pattern->WoLPattern.WoLBitMapPattern.PatternSize);

        patternPayloadSize = max(patternPayloadSize, sizeof(NDIS_PM_WOL_PATTERN));
        patternPayloadSize = patternPayloadSize - sizeof(NDIS_PM_WOL_PATTERN);
    }

    status = RtlULongAdd(sizeof(NX_NET_POWER_ENTRY),
                    patternPayloadSize,
                    &totalAllocationSize);
    if (!NT_SUCCESS(status)) {
        LogError(GetRecorderLog(), FLAG_POWER,
            "Unable to compute size requirement for WoL entry %!STATUS!", status);
        return NULL;
    }

    if (InformationBufferLength < (patternPayloadSize + sizeof(NDIS_PM_WOL_PATTERN))) {
        LogError(GetRecorderLog(), FLAG_POWER,
            "Invalid InformationBufferLength %u for WoL entry", InformationBufferLength);
        return NULL;
    }

    pNxWakeEntry = (PNX_NET_POWER_ENTRY)ExAllocatePoolWithTag(NonPagedPoolNx,
                                                            totalAllocationSize,
                                                            NETADAPTERCX_TAG);
    if (pNxWakeEntry == NULL) {
        LogError(GetRecorderLog(), FLAG_POWER, "Allocation for Nx power entry failed");
        return NULL;
    }

    RtlZeroMemory(pNxWakeEntry, totalAllocationSize);
    pNxWakeEntry->Size = totalAllocationSize;

    RtlCopyMemory(&pNxWakeEntry->NdisWoLPattern,
                Pattern,
                sizeof(NDIS_PM_WOL_PATTERN)+patternPayloadSize);

    return pNxWakeEntry;
}

NDIS_STATUS
NxWake::AddWakePattern(
    _In_ NETADAPTER AdapterWdfHandle,
    _In_ NDIS_OID_REQUEST::_REQUEST_DATA::_SET const *SetInformation
    )
/*++
Routine Description:
    Processes addition of Wake patterns. The pattern is presented
    to the driver if it has registered a callback to process it.

Arguments:
    AdapterWdfHandle - NETADAPTER handle
    SetInformation - SET_INFORMATION parameter from the incoming OID

Returns:
    NDIS_STATUS
    Note if driver returns status other than  STATUS_NDIS_PM_WOL_PATTERN_LIST_FULL
    from m_EvtPreviewWakePattern it is treated as NDIS_STATUS_SUCCESS.

--*/
{
    ASSERT (! m_DriverCanAccessWakeSettings);

    PNX_NET_POWER_ENTRY nxWakeEntry;
    NTSTATUS status = STATUS_SUCCESS;
    PNDIS_PM_WOL_PATTERN ndisWolPattern;

    auto & device = *GetNxDeviceFromHandle(m_NxAdapter->GetDevice());

    if (!device.IncreaseWakePatternReference())
    {
        LogInfo(
            GetRecorderLog(),
            FLAG_DEVICE,
            "Rejecting wake pattern because the maximum number of patterns was reached. NETADAPTER=%p",
            m_NxAdapter->GetFxObject());

        return NDIS_STATUS_PM_WOL_PATTERN_LIST_FULL;
    }

    // Make sure we remove the wake pattern reference from the device if something goes wrong
    auto wakePatternReference = wil::scope_exit([&device]() { device.DecreaseWakePatternReference(); });

    ndisWolPattern = (PNDIS_PM_WOL_PATTERN)SetInformation->InformationBuffer;

    //
    // Allocate and make a copy before invoking the callback to avoid
    // failure after driver accepts a pattern
    //
    nxWakeEntry = CreateWakePatternEntry(ndisWolPattern,
                                SetInformation->InformationBufferLength);
    if (nxWakeEntry == NULL)
    {
        return NdisConvertNtStatusToNdisStatus(STATUS_INSUFFICIENT_RESOURCES);
    }

    //
    // Change nxWakeEntry to see if it is enabled..
    //
    UpdatePatternEntryEnabledField(nxWakeEntry);

    //
    // Invoke optional callback
    //
    if (m_EvtPreviewWakePattern != NULL)
    {
        m_DriverCanAccessWakeSettings = TRUE;
        status = m_EvtPreviewWakePattern(AdapterWdfHandle,
            GetFxObject(),
            nxWakeEntry->NdisWoLPattern.WoLPacketType,
            &(nxWakeEntry->NdisWoLPattern));
        m_DriverCanAccessWakeSettings = FALSE;

        if (status == STATUS_NDIS_PM_WOL_PATTERN_LIST_FULL)
        {
            //
            // In case driver incorrectly saved a pointer to the pattern
            // help catch it sooner.
            //
            RtlFillMemory(nxWakeEntry, nxWakeEntry->Size, 0xC0);
            ExFreePoolWithTag(nxWakeEntry, NETADAPTERCX_TAG);

            return NDIS_STATUS_PM_WOL_PATTERN_LIST_FULL;
        }
    }

    AddWakePatternEntryToList(nxWakeEntry);

    wakePatternReference.release();

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NxWake::RemoveWakePattern(
    _In_ NDIS_OID_REQUEST::_REQUEST_DATA::_SET const *SetInformation
    )
/*++
Routine Description:
    Processes removal of Wake patterns.

Arguments:
    SetInformation - SET_INFORMATION parameter from the incoming OID

Returns:
    NDIS_STATUS

--*/
{
    ASSERT(! m_DriverCanAccessWakeSettings);

    PNX_NET_POWER_ENTRY nxWakeEntry = RemovePowerEntryByID(
        *((PULONG)SetInformation->InformationBuffer),
        NxPowerEntryTypeWakePattern);

    RtlFillMemory(nxWakeEntry, nxWakeEntry->Size, 0xC0);
    ExFreePoolWithTag(nxWakeEntry, NETADAPTERCX_TAG);

    auto & device = *GetNxDeviceFromHandle(m_NxAdapter->GetDevice());
    device.DecreaseWakePatternReference();

    return NDIS_STATUS_SUCCESS;
}

VOID
NxWake::AddWakePatternEntryToList(
    _In_ PNX_NET_POWER_ENTRY Entry
)
/*++
Routine Description:
    Adds an entry to the list and increments count.

Arguments:
    Entry - Entry to be added to the list.

Returns:
    VOID

--*/
{
    m_WakeListCount++;
    PushEntryList(&m_WakeListHead, &Entry->ListEntry);
}

PNX_NET_POWER_ENTRY
NxWake::RemovePowerEntryByID(
    _In_ ULONG               PatternID,
    _In_ NX_POWER_ENTRY_TYPE NxPowerEntryType
)
/*++
Routine Description:
    Iterates through the list of nx power entries and pops the entry that matches
    the PatternID. NDIS guarantees that the PatternID for a given power entry type
    are unique for each miniport.

Arguments:
    PatternID - Unique identifier of the pattern.
    NxPowerEntryType - Type to be removed. Wake pattern or protocol offload

Returns:
    Wake entry if found. NULL if not found.

--*/
{
    PSINGLE_LIST_ENTRY listEntry;
    PSINGLE_LIST_ENTRY prevEntry = NULL;
    PNX_NET_POWER_ENTRY nxEntry;
    ULONG *countToDecrement = NULL;

    if (NxPowerEntryType == NxPowerEntryTypeWakePattern) {
        prevEntry = &m_WakeListHead;
        countToDecrement = &m_WakeListCount;
        ASSERT(m_WakeListCount != 0);
    }
    else if (NxPowerEntryType == NxPowerEntryTypeProtocolOffload) {
        prevEntry = &m_ProtocolOffloadListHead;
        countToDecrement = &m_ProtocolOffloadListCount;
        ASSERT(m_ProtocolOffloadListCount != 0);
    }
    else {
        NT_ASSERTMSG("Invalid NxEntryType", 0);
    }

    listEntry = prevEntry->Next;

    while (listEntry != NULL) {
        nxEntry = CONTAINING_RECORD(listEntry, NX_NET_POWER_ENTRY, ListEntry);
        if (GetPowerEntryID(nxEntry, NxPowerEntryType) == PatternID) {
            (*countToDecrement)--;
            PopEntryList(prevEntry);
            return nxEntry;
        }
        prevEntry = listEntry;
        listEntry = listEntry->Next;
    }

    return NULL;
}

NDIS_STATUS
NxWake::SetParameters(
    _In_ PNDIS_PM_PARAMETERS PmParams
)
/*++
Routine Description:
    Stores the incoming the NDIS_PM_PARAMETERS and if the Wake pattern
    have changed then it updates the Wake patterns to reflect the change

Arguments:
    PmParams - Pointer to the NDIS PM PARAMETERS

Returns:
    NDIS_STATUS that is returned to NDIS.sys
--*/
{
    PSINGLE_LIST_ENTRY listEntry;
    PNX_NET_POWER_ENTRY nxPowerEntry;
    BOOLEAN updateWakePatterns = TRUE;
    BOOLEAN updateProtocolOffload = TRUE;

    ASSERT (m_DriverCanAccessWakeSettings == FALSE);

    if (m_PMParameters.EnabledWoLPacketPatterns ==
            PmParams->EnabledWoLPacketPatterns) {
        updateWakePatterns = FALSE;
    }

    if (m_PMParameters.EnabledProtocolOffloads ==
            PmParams->EnabledProtocolOffloads) {
        updateProtocolOffload = FALSE;
    }

    RtlCopyMemory(&m_PMParameters,
                PmParams,
                sizeof(m_PMParameters));

    if (updateWakePatterns) {
        listEntry = m_WakeListHead.Next;
        while (listEntry != NULL) {
            nxPowerEntry = CONTAINING_RECORD(listEntry, NX_NET_POWER_ENTRY, ListEntry);
            UpdatePatternEntryEnabledField(nxPowerEntry);
            listEntry = listEntry->Next;
        }
    }

    if (updateProtocolOffload) {
        listEntry = m_ProtocolOffloadListHead.Next;
        while (listEntry != NULL) {
            nxPowerEntry = CONTAINING_RECORD(listEntry, NX_NET_POWER_ENTRY, ListEntry);
            UpdateProtocolOffloadEntryEnabledField(nxPowerEntry);
            listEntry = listEntry->Next;
        }
    }

    return NDIS_STATUS_SUCCESS;
}

VOID
NxWake::UpdateProtocolOffloadEntryEnabledField(
    _In_ PNX_NET_POWER_ENTRY Entry
    )
/*++
Routine Description:
    Updates the protocol offload entry's Enabled field based on the
    NDIS_PM_PARAMETERS.

Arguments:
    Entry  -The wake entry to be updated.

Returns:
    VOID
--*/
{
    Entry->Enabled = FALSE;

    switch (Entry->NdisProtocolOffload.ProtocolOffloadType) {
    case NdisPMProtocolOffloadIdIPv4ARP:
        Entry->Enabled = !!(m_PMParameters.EnabledProtocolOffloads & NDIS_PM_PROTOCOL_OFFLOAD_ARP_ENABLED);
        break;
    case NdisPMProtocolOffloadIdIPv6NS:
        Entry->Enabled = !!(m_PMParameters.EnabledProtocolOffloads & NDIS_PM_PROTOCOL_OFFLOAD_NS_ENABLED);
        break;
    case NdisPMProtocolOffload80211RSNRekey:
        Entry->Enabled = !!(m_PMParameters.EnabledProtocolOffloads & NDIS_PM_PROTOCOL_OFFLOAD_80211_RSN_REKEY_ENABLED);
        break;
    default:
        NT_ASSERTMSG("Unexpected protocol offload type", 0);
        break;
    }
}

VOID
NxWake::UpdatePatternEntryEnabledField(
    _In_ PNX_NET_POWER_ENTRY Entry
)
/*++
Routine Description:
    Updates the Wake entry's Enabled field based on the NDIS_PM_PARAMETERS.

Arguments:
    Entry  -The wake entry to be updated.

Returns:
    VOID
--*/
{
    Entry->Enabled = FALSE;

    switch (Entry->NdisWoLPattern.WoLPacketType) {
    case NdisPMWoLPacketBitmapPattern:
        if (m_PMParameters.EnabledWoLPacketPatterns & NDIS_PM_WOL_BITMAP_PATTERN_ENABLED) {
            Entry->Enabled = TRUE;
        }
        break;
    case NdisPMWoLPacketMagicPacket:
        if (m_PMParameters.EnabledWoLPacketPatterns & NDIS_PM_WOL_MAGIC_PACKET_ENABLED) {
            Entry->Enabled = TRUE;
        }
        break;
    case NdisPMWoLPacketIPv4TcpSyn:
        if (m_PMParameters.EnabledWoLPacketPatterns & NDIS_PM_WOL_IPV4_TCP_SYN_ENABLED) {
            Entry->Enabled = TRUE;
        }
        break;
    case NdisPMWoLPacketIPv6TcpSyn:
        if (m_PMParameters.EnabledWoLPacketPatterns & NDIS_PM_WOL_IPV6_TCP_SYN_ENABLED) {
            Entry->Enabled = TRUE;
        }
        break;
    case NdisPMWoLPacketEapolRequestIdMessage:
        if (m_PMParameters.EnabledWoLPacketPatterns & NDIS_PM_WOL_EAPOL_REQUEST_ID_MESSAGE_ENABLED) {
            Entry->Enabled = TRUE;
        }
        break;
    default:
        ASSERT(FALSE);
        break;
    }
}

PNX_NET_POWER_ENTRY
NxWake::GetEntryAtIndex(
    _In_ ULONG Index,
    _In_ NX_POWER_ENTRY_TYPE NxEntryType
    )
/*++
Routine Description:
    Returns the NX_NET_POWER_ENTRY at index Index.

Arguments:
    Index - 0 based index into the list.

Returns:
    PNX_NET_POWER_ENTRY if found. NULL otherwise.

--*/
{
    ULONG i;
    PSINGLE_LIST_ENTRY ple;
    PSINGLE_LIST_ENTRY listHead;
    ULONG maxCount;

    ASSERT(NxEntryType == NxPowerEntryTypeProtocolOffload ||
        NxEntryType == NxPowerEntryTypeWakePattern);

    if (NxEntryType == NxPowerEntryTypeWakePattern) {
        maxCount = m_WakeListCount;
        listHead = &m_WakeListHead;
    }
    else {
        maxCount = m_ProtocolOffloadListCount;
        listHead = &m_ProtocolOffloadListHead;
    }

    if (Index >= maxCount) {
        return NULL;
    }

    for (i = 0, ple = listHead->Next;
        ple != NULL;
        ple = ple->Next, i++) {
        if (i != Index) {
            continue;
        }

        return CONTAINING_RECORD(ple, NX_NET_POWER_ENTRY, ListEntry);
    }

    return NULL;
}

VOID
NxWake::AdapterInitComplete(
    VOID
)
/*++
Routine Description:
    Notification from NxAdapter that initialization is complete to the point of
    no more failures and it is time to enable any driver provided NETPOWERSETTINGS
    cleanup/destroy callbacks.

Arguments:
    VOID

Returns:
    VOID
--*/
{
    ASSERT(m_DriverObjectCallbacksEnabled == FALSE);
    m_DriverObjectCallbacksEnabled = TRUE;
}

BOOLEAN
NxWake::ArePowerSettingsAccessible(
    VOID
)
/*++
Routine Description:
    Checks whether the Power Settings are Accessible

Arguments:
    VOID

Returns:
    BOOLEAN
--*/
{
    WDFDEVICE wdfDevice;
    NxDevice* nxDevice;

    wdfDevice = m_NxAdapter->GetDevice();
    nxDevice = GetNxDeviceFromHandle(wdfDevice);
    return (m_DriverCanAccessWakeSettings || nxDevice->IsDeviceInPowerTransition());
}

ULONG
NxWake::GetWakePatternCountForType(
    _In_ NDIS_PM_WOL_PACKET WakePatternType
)
/*++
Routine Description:
    Gets the Count of Wake Patterns for a particular Wake pattern type

Arguments:
    WakePatternType - the Wake Pattern to be looked up

Returns:
    Count of Patterns for the type specified
--*/
{
    PSINGLE_LIST_ENTRY listEntry;
    PNX_NET_POWER_ENTRY nxEntry;
    ULONG countOfWakeType = 0;

    listEntry = m_WakeListHead.Next;

    while (listEntry != NULL) {
        nxEntry = CONTAINING_RECORD(listEntry, NX_NET_POWER_ENTRY, ListEntry);
        if (nxEntry->NdisWoLPattern.WoLPacketType == WakePatternType) {
            countOfWakeType++;
        }
        listEntry = listEntry->Next;
    }

    return countOfWakeType;
}

ULONG
NxWake::GetProtocolOffloadCountForType(
    _In_ NDIS_PM_PROTOCOL_OFFLOAD_TYPE NdisOffloadType
)
/*++
Routine Description:
    Gets the count of protocol offloads for a particular offload type

Arguments:
    WakePatternType - the Wake Pattern to be looked up

Returns:
    Count of Patterns for the type specified
--*/
{
    PSINGLE_LIST_ENTRY listEntry;
    PNX_NET_POWER_ENTRY nxEntry;
    ULONG countOfType = 0;

    listEntry = m_ProtocolOffloadListHead.Next;

    while (listEntry != NULL) {
        nxEntry = CONTAINING_RECORD(listEntry, NX_NET_POWER_ENTRY, ListEntry);
        if (nxEntry->NdisProtocolOffload.ProtocolOffloadType == NdisOffloadType) {
            countOfType++;
        }
        listEntry = listEntry->Next;
    }

    return countOfType;
}

