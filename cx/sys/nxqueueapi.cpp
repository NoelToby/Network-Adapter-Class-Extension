// Copyright (C) Microsoft Corporation. All rights reserved.

#include "Nx.hpp"

#include <NxApi.hpp>
#include <NetRingBuffer.h>
#include <NetPacket.h>

#include "NxQueueApi.tmh"

#include "NxAdapter.hpp"
#include "NxQueue.hpp"
#include "verifier.hpp"
#include "version.hpp"

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NETEXPORT(NetTxQueueCreate)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PNETTXQUEUE_INIT NetTxQueueInit,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES TxQueueAttributes,
    _In_ PNET_PACKET_QUEUE_CONFIG Configuration,
    _Out_ NETPACKETQUEUE* TxQueue
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);
    auto & InitContext = *reinterpret_cast<QUEUE_CREATION_CONTEXT*>(NetTxQueueInit);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);
    Verifier_VerifyQueueInitContext(pNxPrivateGlobals, &InitContext);
    Verifier_VerifyTypeSize(pNxPrivateGlobals, Configuration);

    // A NETPACKETQUEUE's parent is always the NETADAPTER
    Verifier_VerifyObjectAttributesParentIsNull(pNxPrivateGlobals, TxQueueAttributes);
    Verifier_VerifyObjectAttributesContextSize(pNxPrivateGlobals, TxQueueAttributes, MAXULONG);
    Verifier_VerifyNetPacketQueueConfiguration(pNxPrivateGlobals, Configuration);

    *TxQueue = nullptr;

    WDF_OBJECT_ATTRIBUTES objectAttributes;
    CFX_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&objectAttributes, NxTxQueue);
    objectAttributes.ParentObject = InitContext.Adapter->GetFxObject();

    wil::unique_wdf_object object;
    CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfObjectCreate(&objectAttributes, &object),
        "WdfObjectCreate for NxTxQueue failed.");

    NxTxQueue * txQueue = new (NxTxQueue::_FromFxBaseObject(object.get()))
        NxTxQueue (object.get(), InitContext, InitContext.QueueId, *Configuration);

    CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
        txQueue->Initialize(InitContext),
        "Tx queue creation failed. NxPrivateGlobals=%p", pNxPrivateGlobals);

    if (TxQueueAttributes != WDF_NO_OBJECT_ATTRIBUTES)
    {
        CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
            WdfObjectAllocateContext(object.get(), TxQueueAttributes, NULL),
            "Failed to allocate client context. NxQueue=%p", txQueue);
    }

    // Note: We cannot have failure points after we allocate the client's context, otherwise
    // they might get their EvtCleanupContext callback even for a failed queue creation

    InitContext.CreatedQueueObject = wistd::move(object);
    *TxQueue = static_cast<NETPACKETQUEUE>(
        InitContext.CreatedQueueObject.get());

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(HIGH_LEVEL)
WDFAPI
VOID
NETEXPORT(NetTxQueueNotifyMoreCompletedPacketsAvailable)(
    _In_
    PNET_DRIVER_GLOBALS DriverGlobals,
    _In_
    NETPACKETQUEUE TxQueue
    )
{
    Verifier_VerifyPrivateGlobals(GetPrivateGlobals(DriverGlobals));

    GetTxQueueFromHandle(TxQueue)->NotifyMorePacketsAvailable();
}

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
ULONG
NETEXPORT(NetTxQueueInitGetQueueId)(
    _In_
    PNET_DRIVER_GLOBALS DriverGlobals,
    _In_
    PNETTXQUEUE_INIT NetTxQueueInit
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);
    auto InitContext = reinterpret_cast<QUEUE_CREATION_CONTEXT*>(NetTxQueueInit);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);
    Verifier_VerifyQueueInitContext(pNxPrivateGlobals, InitContext);

    return InitContext->QueueId;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
PCNET_DATAPATH_DESCRIPTOR
NETEXPORT(NetTxQueueGetDatapathDescriptor)(
    _In_
    PNET_DRIVER_GLOBALS DriverGlobals,
    _In_
    NETPACKETQUEUE NetTxQueue
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);

    return GetTxQueueFromHandle(NetTxQueue)->GetPacketRingBufferSet();
}

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NETEXPORT(NetTxQueueInitAddPacketContextAttributes)(
    _In_    PNET_DRIVER_GLOBALS             DriverGlobals,
    _Inout_ PNETTXQUEUE_INIT                NetTxQueueInit,
    _In_    PNET_PACKET_CONTEXT_ATTRIBUTES  PacketContextAttributes
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);
    Verifier_VerifyNetPacketContextAttributes(pNxPrivateGlobals, PacketContextAttributes);

    QUEUE_CREATION_CONTEXT *queueCreationContext = reinterpret_cast<QUEUE_CREATION_CONTEXT *>(NetTxQueueInit);

    return NxQueue::NetQueueInitAddPacketContextAttributes(queueCreationContext, PacketContextAttributes);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
PNET_PACKET_CONTEXT_TOKEN
NETEXPORT(NetTxQueueGetPacketContextToken)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE NetTxQueue,
    _In_ PCNET_CONTEXT_TYPE_INFO ContextTypeInfo
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);

    return GetTxQueueFromHandle(NetTxQueue)->GetPacketContextTokenFromTypeInfo(ContextTypeInfo);
}

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NETEXPORT(NetRxQueueCreate)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PNETRXQUEUE_INIT NetRxQueueInit,
    _In_opt_ PWDF_OBJECT_ATTRIBUTES RxQueueAttributes,
    _In_ PNET_PACKET_QUEUE_CONFIG Configuration,
    _Out_ NETPACKETQUEUE* RxQueue
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);

    auto & InitContext = *reinterpret_cast<QUEUE_CREATION_CONTEXT*>(NetRxQueueInit);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);
    Verifier_VerifyQueueInitContext(pNxPrivateGlobals, &InitContext);
    Verifier_VerifyTypeSize(pNxPrivateGlobals, Configuration);

    // A NETPACKETQUEUE's parent is always the NETADAPTER
    Verifier_VerifyObjectAttributesParentIsNull(pNxPrivateGlobals, RxQueueAttributes);
    Verifier_VerifyObjectAttributesContextSize(pNxPrivateGlobals, RxQueueAttributes, MAXULONG);
    Verifier_VerifyNetPacketQueueConfiguration(pNxPrivateGlobals, Configuration);

    *RxQueue = nullptr;

    WDF_OBJECT_ATTRIBUTES objectAttributes;
    CFX_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&objectAttributes, NxRxQueue);
    objectAttributes.ParentObject = InitContext.Adapter->GetFxObject();

    wil::unique_wdf_object object;
    CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfObjectCreate(&objectAttributes, &object),
        "WdfObjectCreate for NxTxQueue failed.");

    NxRxQueue * rxQueue = new (NxRxQueue::_FromFxBaseObject(object.get()))
    NxRxQueue(object.get(), InitContext, InitContext.QueueId, *Configuration);

    CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
        rxQueue->Initialize(InitContext),
        "Rx queue creation failed. NxPrivateGlobals=%p", pNxPrivateGlobals);

    if (RxQueueAttributes != WDF_NO_OBJECT_ATTRIBUTES)
    {
        CX_RETURN_IF_NOT_NT_SUCCESS_MSG(
            WdfObjectAllocateContext(object.get(), RxQueueAttributes, NULL),
            "Failed to allocate client context. NxQueue=%p", rxQueue);
    }

    // Note: We cannot have failure points after we allocate the client's context, otherwise
    // they might get their EvtCleanupContext callback even for a failed queue creation

    InitContext.CreatedQueueObject = wistd::move(object);
    *RxQueue = static_cast<NETPACKETQUEUE>(
        InitContext.CreatedQueueObject.get());

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(HIGH_LEVEL)
WDFAPI
VOID
NETEXPORT(NetRxQueueNotifyMoreReceivedPacketsAvailable)(
    _In_
    PNET_DRIVER_GLOBALS DriverGlobals,
    _In_
    NETPACKETQUEUE RxQueue
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);

    GetRxQueueFromHandle(RxQueue)->NotifyMorePacketsAvailable();
}

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
ULONG
NETEXPORT(NetRxQueueInitGetQueueId)(
    _In_
    PNET_DRIVER_GLOBALS DriverGlobals,
    _In_
    PNETRXQUEUE_INIT NetRxQueueInit
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);
    auto InitContext = reinterpret_cast<QUEUE_CREATION_CONTEXT*>(NetRxQueueInit);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);
    Verifier_VerifyQueueInitContext(pNxPrivateGlobals, InitContext);

    return InitContext->QueueId;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
PCNET_DATAPATH_DESCRIPTOR
NETEXPORT(NetRxQueueGetDatapathDescriptor)(
    _In_
    PNET_DRIVER_GLOBALS DriverGlobals,
    _In_
    NETPACKETQUEUE NetRxQueue
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);

    return GetRxQueueFromHandle(NetRxQueue)->GetPacketRingBufferSet();
}

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NETEXPORT(NetRxQueueInitAddPacketContextAttributes)(
    _In_    PNET_DRIVER_GLOBALS             DriverGlobals,
    _Inout_ PNETRXQUEUE_INIT                NetRxQueueInit,
    _In_    PNET_PACKET_CONTEXT_ATTRIBUTES  PacketContextAttributes
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);
    Verifier_VerifyNetPacketContextAttributes(pNxPrivateGlobals, PacketContextAttributes);

    QUEUE_CREATION_CONTEXT *queueCreationContext = reinterpret_cast<QUEUE_CREATION_CONTEXT *>(NetRxQueueInit);

    return NxQueue::NetQueueInitAddPacketContextAttributes(queueCreationContext, PacketContextAttributes);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
PNET_PACKET_CONTEXT_TOKEN
NETEXPORT(NetRxQueueGetPacketContextToken)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE NetRxQueue,
    _In_ PCNET_CONTEXT_TYPE_INFO ContextTypeInfo
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);

    return GetRxQueueFromHandle(NetRxQueue)->GetPacketContextTokenFromTypeInfo(ContextTypeInfo);
}

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NETEXPORT(NetTxQueueInitAddPacketExtension)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ PNETTXQUEUE_INIT QueueInit,
    _In_ const PNET_PACKET_EXTENSION ExtensionToAdd
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);
    QUEUE_CREATION_CONTEXT *queueCreationContext = reinterpret_cast<QUEUE_CREATION_CONTEXT *>(QueueInit);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);
    Verifier_VerifyTypeSize(pNxPrivateGlobals, ExtensionToAdd);
    Verifier_VerifyNetPacketExtension(pNxPrivateGlobals, ExtensionToAdd);
    Verifier_VerifyQueueInitContext(pNxPrivateGlobals, queueCreationContext);

    NET_PACKET_EXTENSION_PRIVATE extensionPrivate = {};
    extensionPrivate.Name = ExtensionToAdd->Name;
    extensionPrivate.Size = ExtensionToAdd->ExtensionSize;
    extensionPrivate.Version = ExtensionToAdd->Version;
    extensionPrivate.NonWdfStyleAlignment = ExtensionToAdd->Alignment + 1;

    return NxQueue::NetQueueInitAddPacketExtension(queueCreationContext, &extensionPrivate);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
SIZE_T
NETEXPORT(NetTxQueueGetPacketExtensionOffset)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE NetTxQueue,
    _In_ const PNET_PACKET_EXTENSION_QUERY ExtensionToGet
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);
    Verifier_VerifyTypeSize(pNxPrivateGlobals, ExtensionToGet);
    Verifier_VerifyNetPacketExtensionQuery(pNxPrivateGlobals, ExtensionToGet);

    NxTxQueue *txQueue = GetTxQueueFromHandle(NetTxQueue);

    NET_PACKET_EXTENSION_PRIVATE extensionPrivate = {};
    extensionPrivate.Name = ExtensionToGet->Name;
    extensionPrivate.Version = ExtensionToGet->Version;

    return txQueue->GetPacketExtensionOffset(&extensionPrivate);
}

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NETEXPORT(NetRxQueueInitAddPacketExtension)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ PNETRXQUEUE_INIT QueueInit,
    _In_ const PNET_PACKET_EXTENSION ExtensionToAdd
    )
{
    //
    // store the incoming NET_PACKET_EXTENSION into QUEUE_CREATION_CONTEXT,
    // the incoming extension memory (inc. the string memory) is caller-allocated and
    // expected to be valid until this call is over.
    // During queue creation (Net*QueueCreate), CX would allocate a new internal
    // NET_PACKET_EXTENSION_PRIVATE (apart from the one stored in adapter object)
    // and store it inside the queue object for offset queries
    //

    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);
    QUEUE_CREATION_CONTEXT *queueCreationContext = reinterpret_cast<QUEUE_CREATION_CONTEXT *>(QueueInit);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);
    Verifier_VerifyTypeSize(pNxPrivateGlobals, ExtensionToAdd);
    Verifier_VerifyNetPacketExtension(pNxPrivateGlobals, ExtensionToAdd);
    Verifier_VerifyQueueInitContext(pNxPrivateGlobals, queueCreationContext);

    NET_PACKET_EXTENSION_PRIVATE extensionPrivate = {};
    extensionPrivate.Name = ExtensionToAdd->Name;
    extensionPrivate.Size = ExtensionToAdd->ExtensionSize;
    extensionPrivate.Version = ExtensionToAdd->Version;
    extensionPrivate.NonWdfStyleAlignment = ExtensionToAdd->Alignment + 1;

    return NxQueue::NetQueueInitAddPacketExtension(queueCreationContext, &extensionPrivate);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
SIZE_T
NETEXPORT(NetRxQueueGetPacketExtensionOffset)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE NetRxQueue,
    _In_ const PNET_PACKET_EXTENSION_QUERY ExtensionToGet
    )
{
    auto pNxPrivateGlobals = GetPrivateGlobals(DriverGlobals);

    Verifier_VerifyPrivateGlobals(pNxPrivateGlobals);
    Verifier_VerifyIrqlPassive(pNxPrivateGlobals);
    Verifier_VerifyTypeSize(pNxPrivateGlobals, ExtensionToGet);
    Verifier_VerifyNetPacketExtensionQuery(pNxPrivateGlobals, ExtensionToGet);

    NxRxQueue *rxQueue = GetRxQueueFromHandle(NetRxQueue);

    NET_PACKET_EXTENSION_PRIVATE extensionPrivate = {};
    extensionPrivate.Name = ExtensionToGet->Name;
    extensionPrivate.Version = ExtensionToGet->Version;

    return rxQueue->GetPacketExtensionOffset(&extensionPrivate);
}

