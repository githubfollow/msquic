/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    The per UDP binding (local IP/port and optionally remote IP) state. This
    includes the lookup state for processing a received packet and the list of
    listeners registered.

--*/

#include "precomp.h"
#ifdef QUIC_CLOG
#include "binding.c.clog.h"
#endif

//
// Make sure we will always have enough room to fit our Version Negotiation packet,
// which includes both the global, constant list of supported versions and the
// randomly generated version.
//
#define MAX_VER_NEG_PACKET_LENGTH \
( \
    sizeof(QUIC_VERSION_NEGOTIATION_PACKET) + \
    QUIC_MAX_CONNECTION_ID_LENGTH_INVARIANT + \
    QUIC_MAX_CONNECTION_ID_LENGTH_INVARIANT + \
    sizeof(uint32_t) + \
    (ARRAYSIZE(QuicSupportedVersionList) * sizeof(uint32_t)) \
)
QUIC_STATIC_ASSERT(
    QUIC_DEFAULT_PATH_MTU - 48 >= MAX_VER_NEG_PACKET_LENGTH,
    "Too many supported version numbers! Requires too big of buffer for response!");

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicBindingInitialize(
#ifdef QUIC_COMPARTMENT_ID
    _In_ QUIC_COMPARTMENT_ID CompartmentId,
#endif
    _In_ BOOLEAN ShareBinding,
    _In_ BOOLEAN ServerOwned,
    _In_opt_ const QUIC_ADDR * LocalAddress,
    _In_opt_ const QUIC_ADDR * RemoteAddress,
    _Out_ QUIC_BINDING** NewBinding
    )
{
    QUIC_STATUS Status;
    QUIC_BINDING* Binding;
    uint8_t HashSalt[20];

    Binding = QUIC_ALLOC_NONPAGED(sizeof(QUIC_BINDING));
    if (Binding == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "QUIC_BINDING",
            sizeof(QUIC_BINDING));
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }

    Binding->RefCount = 1;
    Binding->Exclusive = !ShareBinding;
    Binding->ServerOwned = ServerOwned;
    Binding->Connected = RemoteAddress == NULL ? FALSE : TRUE;
    Binding->StatelessOperCount = 0;
    QuicDispatchRwLockInitialize(&Binding->RwLock);
    QuicDispatchLockInitialize(&Binding->ResetTokenLock);
    QuicDispatchLockInitialize(&Binding->StatelessOperLock);
    QuicListInitializeHead(&Binding->Listeners);
    QuicLookupInitialize(&Binding->Lookup);
    QuicHashtableInitializeEx(&Binding->StatelessOperTable, QUIC_HASH_MIN_SIZE);
    QuicListInitializeHead(&Binding->StatelessOperList);

    //
    // Random reserved version number for version negotation.
    //
    QuicRandom(sizeof(uint32_t), &Binding->RandomReservedVersion);
    Binding->RandomReservedVersion =
        (Binding->RandomReservedVersion & ~QUIC_VERSION_RESERVED_MASK) |
        QUIC_VERSION_RESERVED;

    QuicRandom(sizeof(HashSalt), HashSalt);
    Status =
        QuicHashCreate(
            QUIC_HASH_SHA256,
            HashSalt,
            sizeof(HashSalt),
            &Binding->ResetTokenHash);
    if (QUIC_FAILED(Status)) {
        QuicTraceEvent(
            BindingErrorStatus,
            "[bind][%p] ERROR, %u, %s.",
            Binding,
            Status,
            "Create reset token hash");
        goto Error;
    }

#ifdef QUIC_COMPARTMENT_ID
    Binding->CompartmentId = CompartmentId;

    BOOLEAN RevertCompartmentId = FALSE;
    QUIC_COMPARTMENT_ID PrevCompartmentId = QuicCompartmentIdGetCurrent();
    if (PrevCompartmentId != CompartmentId) {
        Status = QuicCompartmentIdSetCurrent(CompartmentId);
        if (QUIC_FAILED(Status)) {
            QuicTraceEvent(
                BindingErrorStatus,
                "[bind][%p] ERROR, %u, %s.",
                Binding,
                Status,
                "Set current compartment Id");
            goto Error;
        }
        RevertCompartmentId = TRUE;
    }
#endif

    Status =
        QuicDataPathBindingCreate(
            MsQuicLib.Datapath,
            LocalAddress,
            RemoteAddress,
            Binding,
            &Binding->DatapathBinding);

#ifdef QUIC_COMPARTMENT_ID
    if (RevertCompartmentId) {
        (void)QuicCompartmentIdSetCurrent(PrevCompartmentId);
    }
#endif

    if (QUIC_FAILED(Status)) {
        QuicTraceEvent(
            BindingErrorStatus,
            "[bind][%p] ERROR, %u, %s.",
            Binding,
            Status,
            "Create datapath binding");
        goto Error;
    }

    QUIC_ADDR DatapathLocalAddr, DatapathRemoteAddr;
    QuicDataPathBindingGetLocalAddress(Binding->DatapathBinding, &DatapathLocalAddr);
    QuicDataPathBindingGetRemoteAddress(Binding->DatapathBinding, &DatapathRemoteAddr);
    QuicTraceEvent(
        BindingCreated,
        "[bind][%p] Created, Udp=%p LocalAddr=%!SOCKADDR! RemoteAddr=%!SOCKADDR!",
        Binding,
        Binding->DatapathBinding,
        LOG_ADDR_LEN(DatapathLocalAddr),
        LOG_ADDR_LEN(DatapathRemoteAddr),
        (uint8_t*)&DatapathLocalAddr,
        (uint8_t*)&DatapathRemoteAddr);

    *NewBinding = Binding;
    Status = QUIC_STATUS_SUCCESS;

Error:

    if (QUIC_FAILED(Status)) {
        if (Binding != NULL) {
            QuicHashFree(Binding->ResetTokenHash);
            QuicLookupUninitialize(&Binding->Lookup);
            QuicHashtableUninitialize(&Binding->StatelessOperTable);
            QuicDispatchLockUninitialize(&Binding->StatelessOperLock);
            QuicDispatchLockUninitialize(&Binding->ResetTokenLock);
            QuicDispatchRwLockUninitialize(&Binding->RwLock);
            QUIC_FREE(Binding);
        }
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicBindingUninitialize(
    _In_ QUIC_BINDING* Binding
    )
{
    QuicTraceEvent(
        BindingCleanup,
        "[bind][%p] Cleaning up",
        Binding);

    QUIC_TEL_ASSERT(Binding->RefCount == 0);
    QUIC_TEL_ASSERT(QuicListIsEmpty(&Binding->Listeners));

    //
    // Delete the datapath binding. This function blocks until all receive
    // upcalls have completed.
    //
    QuicDataPathBindingDelete(Binding->DatapathBinding);

    //
    // Clean up any leftover stateless operations being tracked.
    //
    while (!QuicListIsEmpty(&Binding->StatelessOperList)) {
        QUIC_STATELESS_CONTEXT* StatelessCtx =
            QUIC_CONTAINING_RECORD(
                QuicListRemoveHead(&Binding->StatelessOperList),
                QUIC_STATELESS_CONTEXT,
                ListEntry);
        Binding->StatelessOperCount--;
        QuicHashtableRemove(
            &Binding->StatelessOperTable,
            &StatelessCtx->TableEntry,
            NULL);
        QUIC_DBG_ASSERT(StatelessCtx->IsProcessed);
        QuicPoolFree(
            &StatelessCtx->Worker->StatelessContextPool,
            StatelessCtx);
    }
    QUIC_DBG_ASSERT(Binding->StatelessOperCount == 0);
    QUIC_DBG_ASSERT(Binding->StatelessOperTable.NumEntries == 0);

    QuicHashFree(Binding->ResetTokenHash);
    QuicLookupUninitialize(&Binding->Lookup);
    QuicDispatchLockUninitialize(&Binding->StatelessOperLock);
    QuicHashtableUninitialize(&Binding->StatelessOperTable);
    QuicDispatchLockUninitialize(&Binding->ResetTokenLock);
    QuicDispatchRwLockUninitialize(&Binding->RwLock);

    QuicTraceEvent(
        BindingDestroyed,
        "[bind][%p] Destroyed",
        Binding);
    QUIC_FREE(Binding);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBindingTraceRundown(
    _In_ QUIC_BINDING* Binding
    )
{
    // TODO - Trace datapath binding

    QUIC_ADDR DatapathLocalAddr, DatapathRemoteAddr;
    QuicDataPathBindingGetLocalAddress(Binding->DatapathBinding, &DatapathLocalAddr);
    QuicDataPathBindingGetRemoteAddress(Binding->DatapathBinding, &DatapathRemoteAddr);
    QuicTraceEvent(
        BindingRundown,
        "[bind][%p] Rundown, Udp=%p LocalAddr=%!SOCKADDR! RemoteAddr=%!SOCKADDR!",
        Binding,
        Binding->DatapathBinding,
        LOG_ADDR_LEN(DatapathLocalAddr),
        LOG_ADDR_LEN(DatapathRemoteAddr),
        (uint8_t*)&DatapathLocalAddr,
        (uint8_t*)&DatapathRemoteAddr);

    QuicDispatchRwLockAcquireShared(&Binding->RwLock);

    for (QUIC_LIST_ENTRY* Link = Binding->Listeners.Flink;
        Link != &Binding->Listeners;
        Link = Link->Flink) {
        QuicListenerTraceRundown(
            QUIC_CONTAINING_RECORD(Link, QUIC_LISTENER, Link));
    }

    QuicDispatchRwLockReleaseShared(&Binding->RwLock);
}

//
// Returns TRUE if there are any registered listeners on this binding.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingHasListenerRegistered(
    _In_ const QUIC_BINDING* const Binding
    )
{
    return !QuicListIsEmpty(&Binding->Listeners);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
QuicBindingRegisterListener(
    _In_ QUIC_BINDING* Binding,
    _In_ QUIC_LISTENER* NewListener
    )
{
    BOOLEAN AddNewListener = TRUE;
    BOOLEAN MaximizeLookup = FALSE;

    const QUIC_ADDR* NewAddr = &NewListener->LocalAddress;
    const BOOLEAN NewWildCard = NewListener->WildCard;
    const QUIC_ADDRESS_FAMILY NewFamily = QuicAddrGetFamily(NewAddr);

    QuicDispatchRwLockAcquireExclusive(&Binding->RwLock);

    //
    // For a single binding, listeners are saved in a linked list, sorted by
    // family first, in decending order {AF_INET6, AF_INET, AF_UNSPEC}, and then
    // specific addresses followed by wild card addresses. Insertion of a new
    // listener with a given IP/ALPN go at the end of the existing family group,
    // only if there isn't a direct match prexisting in the list.
    //

    QUIC_LIST_ENTRY* Link;
    for (Link = Binding->Listeners.Flink;
        Link != &Binding->Listeners;
        Link = Link->Flink) {

        const QUIC_LISTENER* ExistingListener =
            QUIC_CONTAINING_RECORD(Link, QUIC_LISTENER, Link);
        const QUIC_ADDR* ExistingAddr = &ExistingListener->LocalAddress;
        const BOOLEAN ExistingWildCard = ExistingListener->WildCard;
        const QUIC_ADDRESS_FAMILY ExistingFamily = QuicAddrGetFamily(ExistingAddr);

        if (NewFamily > ExistingFamily) {
            break; // End of possible family matches. Done searching.
        } else if (NewFamily != ExistingFamily) {
            continue;
        }

        if (!NewWildCard && ExistingWildCard) {
            break; // End of specific address matches. Done searching.
        } else if (NewWildCard != ExistingWildCard) {
            continue;
        }

        if (NewFamily != AF_UNSPEC && !QuicAddrCompareIp(NewAddr, ExistingAddr)) {
            continue;
        }

        if (QuicSessionHasAlpnOverlap(NewListener->Session, ExistingListener->Session)) {
            QuicTraceLogWarning(
                BindingListenerAlreadyRegistered,
                "[bind][%p] Listener (%p) already registered on ALPN",
                Binding, ExistingListener);
            AddNewListener = FALSE;
            break;
        }
    }

    if (AddNewListener) {
        MaximizeLookup = QuicListIsEmpty(&Binding->Listeners);

        //
        // If we search all the way back to the head of the list, just insert
        // the new listener at the end of the list. Otherwise, we terminated
        // prematurely based on sort order. Insert the new listener right before
        // the current Link.
        //
        if (Link == &Binding->Listeners) {
            QuicListInsertTail(&Binding->Listeners, &NewListener->Link);
        } else {
            NewListener->Link.Flink = Link;
            NewListener->Link.Blink = Link->Blink;
            NewListener->Link.Blink->Flink = &NewListener->Link;
            Link->Blink = &NewListener->Link;
        }
    }

    QuicDispatchRwLockReleaseExclusive(&Binding->RwLock);

    if (MaximizeLookup &&
        !QuicLookupMaximizePartitioning(&Binding->Lookup)) {
        QuicBindingUnregisterListener(Binding, NewListener);
        AddNewListener = FALSE;
    }

    return AddNewListener;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Success_(return != NULL)
QUIC_LISTENER*
QuicBindingGetListener(
    _In_ QUIC_BINDING* Binding,
    _Inout_ QUIC_NEW_CONNECTION_INFO* Info
    )
{
    QUIC_LISTENER* Listener = NULL;

    const QUIC_ADDR* Addr = Info->LocalAddress;
    const QUIC_ADDRESS_FAMILY Family = QuicAddrGetFamily(Addr);

    QuicDispatchRwLockAcquireShared(&Binding->RwLock);

    for (QUIC_LIST_ENTRY* Link = Binding->Listeners.Flink;
        Link != &Binding->Listeners;
        Link = Link->Flink) {

        QUIC_LISTENER* ExistingListener =
            QUIC_CONTAINING_RECORD(Link, QUIC_LISTENER, Link);
        const QUIC_ADDR* ExistingAddr = &ExistingListener->LocalAddress;
        const BOOLEAN ExistingWildCard = ExistingListener->WildCard;
        const QUIC_ADDRESS_FAMILY ExistingFamily = QuicAddrGetFamily(ExistingAddr);

        if (ExistingFamily != AF_UNSPEC) {
            if (Family != ExistingFamily ||
                (!ExistingWildCard && !QuicAddrCompareIp(Addr, ExistingAddr))) {
                continue; // No IP match.
            }
        }

        if (QuicSessionMatchesAlpn(ExistingListener->Session, Info)) {
            if (QuicRundownAcquire(&ExistingListener->Rundown)) {
                Listener = ExistingListener;
            }
            goto Done;
        }
    }

Done:

    QuicDispatchRwLockReleaseShared(&Binding->RwLock);

    return Listener;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicBindingUnregisterListener(
    _In_ QUIC_BINDING* Binding,
    _In_ QUIC_LISTENER* Listener
    )
{
    QuicDispatchRwLockAcquireExclusive(&Binding->RwLock);
    QuicListEntryRemove(&Listener->Link);
    QuicDispatchRwLockReleaseExclusive(&Binding->RwLock);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingAddSourceConnectionID(
    _In_ QUIC_BINDING* Binding,
    _In_ QUIC_CID_HASH_ENTRY* SourceCid
    )
{
    return QuicLookupAddLocalCid(&Binding->Lookup, SourceCid, NULL);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBindingRemoveSourceConnectionID(
    _In_ QUIC_BINDING* Binding,
    _In_ QUIC_CID_HASH_ENTRY* SourceCid
    )
{
    QuicLookupRemoveLocalCid(&Binding->Lookup, SourceCid);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBindingRemoveConnection(
    _In_ QUIC_BINDING* Binding,
    _In_ QUIC_CONNECTION* Connection
    )
{
    if (Connection->RemoteHashEntry != NULL) {
        QuicLookupRemoveRemoteHash(&Binding->Lookup, Connection->RemoteHashEntry);
    }
    QuicLookupRemoveLocalCids(&Binding->Lookup, Connection);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBindingMoveSourceConnectionIDs(
    _In_ QUIC_BINDING* BindingSrc,
    _In_ QUIC_BINDING* BindingDest,
    _In_ QUIC_CONNECTION* Connection
    )
{
    QuicLookupMoveLocalConnectionIDs(
        &BindingSrc->Lookup, &BindingDest->Lookup, Connection);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBindingOnConnectionHandshakeConfirmed(
    _In_ QUIC_BINDING* Binding,
    _In_ QUIC_CONNECTION* Connection
    )
{
    if (Connection->RemoteHashEntry != NULL) {
        QuicLookupRemoveRemoteHash(&Binding->Lookup, Connection->RemoteHashEntry);
    }
}

//
// This attempts to add a new stateless operation (for a given remote endpoint)
// to the tracking structures in the binding. It first ages out any old
// operations that might have expired. Then it adds the new operation only if
// the remote address isn't already in the table.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATELESS_CONTEXT*
QuicBindingCreateStatelessOperation(
    _In_ QUIC_BINDING* Binding,
    _In_ QUIC_WORKER* Worker,
    _In_ QUIC_RECV_DATAGRAM* Datagram
    )
{
    uint32_t TimeMs = QuicTimeMs32();
    const QUIC_ADDR* RemoteAddress = &Datagram->Tuple->RemoteAddress;
    uint32_t Hash = QuicAddrHash(RemoteAddress);
    QUIC_STATELESS_CONTEXT* StatelessCtx = NULL;

    QuicDispatchLockAcquire(&Binding->StatelessOperLock);

    //
    // Age out all expired operation contexts.
    //
    while (!QuicListIsEmpty(&Binding->StatelessOperList)) {
        QUIC_STATELESS_CONTEXT* OldStatelessCtx =
            QUIC_CONTAINING_RECORD(
                Binding->StatelessOperList.Flink,
                QUIC_STATELESS_CONTEXT,
                ListEntry);

        if (QuicTimeDiff32(OldStatelessCtx->CreationTimeMs, TimeMs) <
            QUIC_STATELESS_OPERATION_EXPIRATION_MS) {
            break;
        }

        //
        // The operation is expired. Remove it from the tracking structures.
        //
        OldStatelessCtx->IsExpired = TRUE;
        QuicHashtableRemove(
            &Binding->StatelessOperTable,
            &OldStatelessCtx->TableEntry,
            NULL);
        QuicListEntryRemove(&OldStatelessCtx->ListEntry);
        Binding->StatelessOperCount--;

        //
        // If it's also processed, free it.
        //
        if (OldStatelessCtx->IsProcessed) {
            QuicPoolFree(
                &OldStatelessCtx->Worker->StatelessContextPool,
                OldStatelessCtx);
        }
    }

    if (Binding->StatelessOperCount >= QUIC_MAX_BINDING_STATELESS_OPERATIONS) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Max binding operations reached");
        goto Exit;
    }

    //
    // Check for pre-existing operations already in the tracking structures.
    //

    QUIC_HASHTABLE_LOOKUP_CONTEXT Context;
    QUIC_HASHTABLE_ENTRY* TableEntry =
        QuicHashtableLookup(&Binding->StatelessOperTable, Hash, &Context);

    while (TableEntry != NULL) {
        const QUIC_STATELESS_CONTEXT* ExistingCtx =
            QUIC_CONTAINING_RECORD(TableEntry, QUIC_STATELESS_CONTEXT, TableEntry);

        if (QuicAddrCompare(&ExistingCtx->RemoteAddress, RemoteAddress)) {
            QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
                "Already in stateless oper table");
            goto Exit;
        }

        TableEntry =
            QuicHashtableLookupNext(&Binding->StatelessOperTable, &Context);
    }

    //
    // Not already in the tracking structures, so allocate and insert a new one.
    //

    StatelessCtx =
        (QUIC_STATELESS_CONTEXT*)QuicPoolAlloc(&Worker->StatelessContextPool);
    if (StatelessCtx == NULL) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Alloc failure for stateless oper ctx");
        goto Exit;
    }

    StatelessCtx->Binding = Binding;
    StatelessCtx->Worker = Worker;
    StatelessCtx->Datagram = Datagram;
    StatelessCtx->CreationTimeMs = TimeMs;
    StatelessCtx->HasBindingRef = FALSE;
    StatelessCtx->IsProcessed = FALSE;
    StatelessCtx->IsExpired = FALSE;
    QuicCopyMemory(&StatelessCtx->RemoteAddress, RemoteAddress, sizeof(QUIC_ADDR));

    QuicHashtableInsert(
        &Binding->StatelessOperTable,
        &StatelessCtx->TableEntry,
        Hash,
        NULL); // TODO - Context?

    QuicListInsertTail(
        &Binding->StatelessOperList,
        &StatelessCtx->ListEntry
        );

    Binding->StatelessOperCount++;

Exit:

    QuicDispatchLockRelease(&Binding->StatelessOperLock);

    return StatelessCtx;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingQueueStatelessOperation(
    _In_ QUIC_BINDING* Binding,
    _In_ QUIC_OPERATION_TYPE OperType,
    _In_ QUIC_RECV_DATAGRAM* Datagram
    )
{
    if (MsQuicLib.WorkerPool == NULL) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "NULL worker pool");
        return FALSE;
    }

    QUIC_WORKER* Worker = QuicLibraryGetWorker();
    if (QuicWorkerIsOverloaded(Worker)) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Worker overloaded (stateless oper)");
        return FALSE;
    }

    QUIC_STATELESS_CONTEXT* Context =
        QuicBindingCreateStatelessOperation(Binding, Worker, Datagram);
    if (Context == NULL) {
        return FALSE;
    }

    QUIC_OPERATION* Oper = QuicOperationAlloc(Worker, OperType);
    if (Oper == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "stateless operation",
            sizeof(QUIC_OPERATION));
        QuicPacketLogDrop(
            Binding,
            QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Alloc failure for stateless operation");
        QuicBindingReleaseStatelessOperation(Context, FALSE);
        return FALSE;
    }

    Oper->STATELESS.Context = Context;
    QuicWorkerQueueOperation(Worker, Oper);

    return TRUE;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicBindingProcessStatelessOperation(
    _In_ uint32_t OperationType,
    _In_ QUIC_STATELESS_CONTEXT* StatelessCtx
    )
{
    QUIC_BINDING* Binding = StatelessCtx->Binding;
    QUIC_RECV_DATAGRAM* RecvDatagram = StatelessCtx->Datagram;
    QUIC_RECV_PACKET* RecvPacket =
        QuicDataPathRecvDatagramToRecvPacket(RecvDatagram);

    QUIC_DBG_ASSERT(RecvPacket->ValidatedHeaderInv);

    QuicTraceEvent(
        BindingExecOper,
        "[bind][%p] Execute: %u",
        Binding,
        OperationType);

    QUIC_DATAPATH_SEND_CONTEXT* SendContext =
        QuicDataPathBindingAllocSendContext(Binding->DatapathBinding, 0);
    if (SendContext == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "stateless send context",
            0);
        goto Exit;
    }

    if (OperationType == QUIC_OPER_TYPE_VERSION_NEGOTIATION) {

        QUIC_DBG_ASSERT(RecvPacket->DestCid != NULL);
        QUIC_DBG_ASSERT(RecvPacket->SourceCid != NULL);

        const uint16_t PacketLength =
            sizeof(QUIC_VERSION_NEGOTIATION_PACKET) +               // Header
            RecvPacket->SourceCidLen +
            sizeof(uint8_t) +
            RecvPacket->DestCidLen +
            sizeof(uint32_t) +                                      // One random version
            ARRAYSIZE(QuicSupportedVersionList) * sizeof(uint32_t); // Our actual supported versions

        QUIC_BUFFER* SendDatagram =
            QuicDataPathBindingAllocSendDatagram(SendContext, PacketLength);
        if (SendDatagram == NULL) {
            QuicTraceEvent(
                AllocFailure,
                "Allocation of '%s' failed. (%llu bytes)",
                "vn datagram",
                PacketLength);
            goto Exit;
        }

        QUIC_VERSION_NEGOTIATION_PACKET* VerNeg =
            (QUIC_VERSION_NEGOTIATION_PACKET*)SendDatagram->Buffer;
        QUIC_DBG_ASSERT(SendDatagram->Length == PacketLength);

        VerNeg->IsLongHeader = TRUE;
        VerNeg->Version = QUIC_VERSION_VER_NEG;

        uint8_t* Buffer = VerNeg->DestCid;
        VerNeg->DestCidLength = RecvPacket->SourceCidLen;
        memcpy(
            Buffer,
            RecvPacket->SourceCid,
            RecvPacket->SourceCidLen);
        Buffer += RecvPacket->SourceCidLen;

        *Buffer = RecvPacket->DestCidLen;
        Buffer++;
        memcpy(
            Buffer,
            RecvPacket->DestCid,
            RecvPacket->DestCidLen);
        Buffer += RecvPacket->DestCidLen;

        uint8_t RandomValue = 0;
        QuicRandom(sizeof(uint8_t), &RandomValue);
        VerNeg->Unused = 0x7F & RandomValue;

        uint32_t* SupportedVersion = (uint32_t*)Buffer;
        SupportedVersion[0] = Binding->RandomReservedVersion;
        for (uint32_t i = 0; i < ARRAYSIZE(QuicSupportedVersionList); ++i) {
            SupportedVersion[1 + i] = QuicSupportedVersionList[i].Number;
        }

        QuicTraceLogVerbose(
            PacketTxVersionNegotiation,
            "[S][TX][-] VN");

    } else if (OperationType == QUIC_OPER_TYPE_STATELESS_RESET) {

        QUIC_DBG_ASSERT(RecvPacket->DestCid != NULL);
        QUIC_DBG_ASSERT(RecvPacket->SourceCid == NULL);

        //
        // There are a few requirements for sending stateless reset packets:
        //
        //   - It must be smaller than the received packet.
        //   - It must be larger than a spec defined minimum (39 bytes).
        //   - It must be sufficiently random so that a middle box cannot easily
        //     detect that it is a stateless reset packet.
        //

        //
        // Add a bit of randomness (3 bits worth) to the packet length.
        //
        uint8_t PacketLength;
        QuicRandom(sizeof(PacketLength), &PacketLength);
        PacketLength >>= 5; // Only drop 5 of the 8 bits of randomness.
        PacketLength += QUIC_RECOMMENDED_STATELESS_RESET_PACKET_LENGTH;

        if (PacketLength >= RecvPacket->BufferLength) {
            //
            // Can't go over the recieve packet's length.
            //
            PacketLength = (uint8_t)RecvPacket->BufferLength - 1;
        }

        QUIC_DBG_ASSERT(PacketLength >= QUIC_MIN_STATELESS_RESET_PACKET_LENGTH);

        QUIC_BUFFER* SendDatagram =
            QuicDataPathBindingAllocSendDatagram(SendContext, PacketLength);
        if (SendDatagram == NULL) {
            QuicTraceEvent(
                AllocFailure,
                "Allocation of '%s' failed. (%llu bytes)",
                "reset datagram",
                PacketLength);
            goto Exit;
        }

        QUIC_SHORT_HEADER_V1* ResetPacket =
            (QUIC_SHORT_HEADER_V1*)SendDatagram->Buffer;
        QUIC_DBG_ASSERT(SendDatagram->Length == PacketLength);

        QuicRandom(
            PacketLength - QUIC_STATELESS_RESET_TOKEN_LENGTH,
            SendDatagram->Buffer);
        ResetPacket->IsLongHeader = FALSE;
        ResetPacket->FixedBit = 1;
        ResetPacket->KeyPhase = RecvPacket->SH->KeyPhase;
        QuicBindingGenerateStatelessResetToken(
            Binding,
            RecvPacket->DestCid,
            SendDatagram->Buffer + PacketLength - QUIC_STATELESS_RESET_TOKEN_LENGTH);

        QuicTraceLogVerbose(
            PacketTxStatelessReset,
            "[S][TX][-] SR %s",
            QuicCidBufToStr(
                SendDatagram->Buffer + PacketLength - QUIC_STATELESS_RESET_TOKEN_LENGTH,
                QUIC_STATELESS_RESET_TOKEN_LENGTH
            ).Buffer);

    } else if (OperationType == QUIC_OPER_TYPE_RETRY) {

        QUIC_DBG_ASSERT(RecvPacket->DestCid != NULL);
        QUIC_DBG_ASSERT(RecvPacket->SourceCid != NULL);

        uint16_t PacketLength = QuicPacketMaxBufferSizeForRetryV1();
        QUIC_BUFFER* SendDatagram =
            QuicDataPathBindingAllocSendDatagram(SendContext, PacketLength);
        if (SendDatagram == NULL) {
            QuicTraceEvent(
                AllocFailure,
                "Allocation of '%s' failed. (%llu bytes)",
                "retry datagram",
                PacketLength);
            goto Exit;
        }

        uint8_t NewDestCid[MSQUIC_CID_MAX_LENGTH];
        QUIC_DBG_ASSERT(sizeof(NewDestCid) >= MsQuicLib.CidTotalLength);
        QuicRandom(sizeof(NewDestCid), NewDestCid);

        QUIC_RETRY_TOKEN_CONTENTS Token = { 0 };
        Token.Authenticated.Timestamp = QuicTimeEpochMs64();

        Token.Encrypted.RemoteAddress = RecvDatagram->Tuple->RemoteAddress;
        QuicCopyMemory(Token.Encrypted.OrigConnId, RecvPacket->DestCid, RecvPacket->DestCidLen);
        Token.Encrypted.OrigConnIdLength = RecvPacket->DestCidLen;

        uint8_t Iv[QUIC_IV_LENGTH];
        if (MsQuicLib.CidTotalLength >= sizeof(Iv)) {
            QuicCopyMemory(Iv, NewDestCid, sizeof(Iv));
            for (uint8_t i = sizeof(Iv); i < MsQuicLib.CidTotalLength; ++i) {
                Iv[i % sizeof(Iv)] ^= NewDestCid[i];
            }
        } else {
            QuicZeroMemory(Iv, sizeof(Iv));
            QuicCopyMemory(Iv, NewDestCid, MsQuicLib.CidTotalLength);
        }

        QuicLockAcquire(&MsQuicLib.StatelessRetryKeysLock);

        QUIC_KEY* StatelessRetryKey = QuicLibraryGetCurrentStatelessRetryKey();
        if (StatelessRetryKey == NULL) {
            QuicLockRelease(&MsQuicLib.StatelessRetryKeysLock);
            goto Exit;
        }

        QUIC_STATUS Status =
            QuicEncrypt(
                StatelessRetryKey,
                Iv,
                sizeof(Token.Authenticated), (uint8_t*) &Token.Authenticated,
                sizeof(Token.Encrypted) + sizeof(Token.EncryptionTag), (uint8_t*)&(Token.Encrypted));

        QuicLockRelease(&MsQuicLib.StatelessRetryKeysLock);
        if (QUIC_FAILED(Status)) {
            goto Exit;
        }

        SendDatagram->Length =
            QuicPacketEncodeRetryV1(
                RecvPacket->LH->Version,
                RecvPacket->SourceCid, RecvPacket->SourceCidLen,
                NewDestCid, MsQuicLib.CidTotalLength,
                RecvPacket->DestCid, RecvPacket->DestCidLen,
                sizeof(Token),
                (uint8_t*)&Token,
                (uint16_t)SendDatagram->Length,
                (uint8_t*)SendDatagram->Buffer);
        QUIC_DBG_ASSERT(SendDatagram->Length != 0);

        QuicTraceLogVerbose(
            PacketTxRetry,
            "[S][TX][-] LH Ver:0x%x DestCid:%s SrcCid:%s Type:R OrigDestCid:%s (Token %hu bytes)",
            RecvPacket->LH->Version,
            QuicCidBufToStr(RecvPacket->SourceCid, RecvPacket->SourceCidLen).Buffer,
            QuicCidBufToStr(NewDestCid, MsQuicLib.CidTotalLength).Buffer,
            QuicCidBufToStr(RecvPacket->DestCid, RecvPacket->DestCidLen).Buffer,
            (uint16_t)sizeof(Token));

    } else {
        QUIC_TEL_ASSERT(FALSE); // Should be unreachable code.
        goto Exit;
    }

    QuicBindingSendFromTo(
        Binding,
        &RecvDatagram->Tuple->LocalAddress,
        &RecvDatagram->Tuple->RemoteAddress,
        SendContext);
    SendContext = NULL;

Exit:

    if (SendContext != NULL) {
        QuicDataPathBindingFreeSendContext(SendContext);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QuicBindingReleaseStatelessOperation(
    _In_ QUIC_STATELESS_CONTEXT* StatelessCtx,
    _In_ BOOLEAN ReturnDatagram
    )
{
    QUIC_BINDING* Binding = StatelessCtx->Binding;

    if (ReturnDatagram) {
        QuicDataPathBindingReturnRecvDatagrams(StatelessCtx->Datagram);
    }
    StatelessCtx->Datagram = NULL;

    QuicDispatchLockAcquire(&Binding->StatelessOperLock);

    StatelessCtx->IsProcessed = TRUE;
    uint8_t FreeCtx = StatelessCtx->IsExpired;

    QuicDispatchLockRelease(&Binding->StatelessOperLock);

    if (StatelessCtx->HasBindingRef) {
        QuicLibraryReleaseBinding(Binding);
    }

    if (FreeCtx) {
        QuicPoolFree(
            &StatelessCtx->Worker->StatelessContextPool,
            StatelessCtx);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingQueueStatelessReset(
    _In_ QUIC_BINDING* Binding,
    _In_ QUIC_RECV_DATAGRAM* Datagram
    )
{
    QUIC_DBG_ASSERT(!Binding->Exclusive);
    QUIC_DBG_ASSERT(!((QUIC_SHORT_HEADER_V1*)Datagram->Buffer)->IsLongHeader);

    if (Datagram->BufferLength <= QUIC_MIN_STATELESS_RESET_PACKET_LENGTH) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Packet too short for stateless reset");
        return FALSE;
    }

    if (Binding->Exclusive) {
        //
        // Can't support stateless reset in exclusive mode, because we don't use
        // a connection ID. Without a connection ID, a stateless reset token
        // cannot be generated.
        //
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "No stateless reset on exclusive binding");
        return FALSE;
    }

    return
        QuicBindingQueueStatelessOperation(
            Binding, QUIC_OPER_TYPE_STATELESS_RESET, Datagram);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingPreprocessDatagram(
    _In_ QUIC_BINDING* Binding,
    _Inout_ QUIC_RECV_DATAGRAM* Datagram,
    _Out_ BOOLEAN* ReleaseDatagram
    )
{
    QUIC_RECV_PACKET* Packet = QuicDataPathRecvDatagramToRecvPacket(Datagram);
    QuicZeroMemory(Packet, sizeof(QUIC_RECV_PACKET));
    Packet->Buffer = Datagram->Buffer;
    Packet->BufferLength = Datagram->BufferLength;

    *ReleaseDatagram = TRUE;

    //
    // Get the destination connection ID from the packet so we can use it for
    // determining delivery partition. All this must be version INDEPENDENT as
    // we haven't done any version validation at this point.
    //

    if (!QuicPacketValidateInvariant(Binding, Packet, !Binding->Exclusive)) {
        return FALSE;
    }

    if (Packet->Invariant->IsLongHeader) {
        //
        // Validate we support this long header packet version.
        //
        if (!QuicIsVersionSupported(Packet->Invariant->LONG_HDR.Version)) {
            if (!QuicBindingHasListenerRegistered(Binding)) {
                QuicPacketLogDrop(Binding, Packet, "No listener to send VN");
            } else {
                *ReleaseDatagram =
                    !QuicBindingQueueStatelessOperation(
                        Binding, QUIC_OPER_TYPE_VERSION_NEGOTIATION, Datagram);
            }
            return FALSE;
        }
    }

    *ReleaseDatagram = FALSE;

    return TRUE;
}

//
// Returns TRUE if the retry token was successfully decrypted and validated.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingValidateRetryToken(
    _In_ const QUIC_BINDING* const Binding,
    _In_ const QUIC_RECV_PACKET* const Packet,
    _In_ uint16_t TokenLength,
    _In_reads_(TokenLength)
        const uint8_t* TokenBuffer
    )
{
    if (TokenLength != sizeof(QUIC_RETRY_TOKEN_CONTENTS)) {
        QuicPacketLogDrop(Binding, Packet, "Invalid Retry Token Length");
        return FALSE;
    }

    QUIC_RETRY_TOKEN_CONTENTS Token;
    if (!QuicRetryTokenDecrypt(Packet, TokenBuffer, &Token)) {
        QuicPacketLogDrop(Binding, Packet, "Retry Token Decryption Failure");
        return FALSE;
    }

    if (Token.Encrypted.OrigConnIdLength > sizeof(Token.Encrypted.OrigConnId)) {
        QuicPacketLogDrop(Binding, Packet, "Invalid Retry Token OrigConnId Length");
        return FALSE;
    }

    const QUIC_RECV_DATAGRAM* Datagram =
        QuicDataPathRecvPacketToRecvDatagram(Packet);
    if (!QuicAddrCompare(&Token.Encrypted.RemoteAddress, &Datagram->Tuple->RemoteAddress)) {
        QuicPacketLogDrop(Binding, Packet, "Retry Token Addr Mismatch");
        return FALSE;
    }

    return TRUE;
}

//
// Returns TRUE if we should respond to the connection attempt with a Retry
// packet.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBindingShouldRetryConnection(
    _In_ const QUIC_BINDING* const Binding,
    _In_ QUIC_RECV_PACKET* Packet,
    _In_ uint16_t TokenLength,
    _In_reads_(TokenLength)
        const uint8_t* Token,
    _Inout_ BOOLEAN* DropPacket
    )
{
    //
    // This is only called once we've determined we can create a new connection.
    // If there is a token, it validates the token. If there is no token, then
    // the function checks to see if the binding currently has too many
    // connections in the handshake state already. If so, it requests the client
    // to retry its connection attempt to prove source address ownership.
    //

    if (TokenLength != 0) {
        //
        // Must always validate the token when provided by the client.
        //
        if (QuicBindingValidateRetryToken(Binding, Packet, TokenLength, Token)) {
            Packet->ValidToken = TRUE;
        } else {
            *DropPacket = TRUE;
        }
        return FALSE;
    }

    uint64_t CurrentMemoryLimit =
        (MsQuicLib.Settings.RetryMemoryLimit * QuicTotalMemory) / UINT16_MAX;

    return MsQuicLib.CurrentHandshakeMemoryUsage >= CurrentMemoryLimit;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_CONNECTION*
QuicBindingCreateConnection(
    _In_ QUIC_BINDING* Binding,
    _In_ const QUIC_RECV_DATAGRAM* const Datagram
    )
{
    //
    // This function returns either a new connection, or an existing
    // connection if a collision is discovered on calling
    // QuicLookupAddRemoteHash.
    //

    QUIC_CONNECTION* Connection = NULL;
    QUIC_RECV_PACKET* Packet = QuicDataPathRecvDatagramToRecvPacket(Datagram);

    QUIC_CONNECTION* NewConnection;
    QUIC_STATUS Status =
        QuicConnInitialize(
            MsQuicLib.UnregisteredSession,
            Datagram,
            &NewConnection);
    if (QUIC_FAILED(Status)) {
        QuicConnRelease(NewConnection, QUIC_CONN_REF_HANDLE_OWNER);
        QuicPacketLogDropWithValue(Binding, Packet,
            "Failed to initialize new connection", Status);
        return NULL;
    }

    BOOLEAN BindingRefAdded = FALSE;
    QUIC_DBG_ASSERT(NewConnection->SourceCids.Next != NULL);
    QUIC_CID_HASH_ENTRY* SourceCid =
        QUIC_CONTAINING_RECORD(
            NewConnection->SourceCids.Next,
            QUIC_CID_HASH_ENTRY,
            Link);

    QuicConnAddRef(NewConnection, QUIC_CONN_REF_LOOKUP_RESULT);

    //
    // Pick a temporary worker to process the client hello and if successful,
    // the connection will later be moved to the correct registration's worker.
    //
    QUIC_WORKER* Worker = QuicLibraryGetWorker();
    if (QuicWorkerIsOverloaded(Worker)) {
        QuicPacketLogDrop(Binding, Packet, "Worker overloaded");
        goto Exit;
    }
    QuicWorkerAssignConnection(Worker, NewConnection);

    //
    // Even though the new connection might not end up being put in this
    // binding's lookup table, it must be completely set up before it is
    // inserted into the table. Once in the table, other threads/processors
    // could immediately be queuing new operations.
    //

    if (!QuicLibraryTryAddRefBinding(Binding)) {
        QuicPacketLogDrop(Binding, QuicDataPathRecvDatagramToRecvPacket(Datagram),
            "Clean up in progress");
        goto Exit;
    }

    BindingRefAdded = TRUE;
    NewConnection->Paths[0].Binding = Binding;

    if (!QuicLookupAddRemoteHash(
            &Binding->Lookup,
            NewConnection,
            &Datagram->Tuple->RemoteAddress,
            Packet->SourceCidLen,
            Packet->SourceCid,
            &Connection)) {
        //
        // Collision with an existing connection or a memory failure.
        //
        if (Connection == NULL) {
            QuicPacketLogDrop(Binding, Packet, "Failed to insert remote hash");
        }
        goto Exit;
    }

    QuicWorkerQueueConnection(NewConnection->Worker, NewConnection);

    return NewConnection;

Exit:

    NewConnection->SourceCids.Next = NULL;
    QUIC_FREE(SourceCid);
    QuicConnRelease(NewConnection, QUIC_CONN_REF_LOOKUP_RESULT);

    if (BindingRefAdded) {
        //
        // The binding ref cannot be released on the receive thread. So, once
        // it has been acquired, we must queue the connection, only to shut it
        // down.
        //
        if (InterlockedCompareExchange16(
                (short*)&Connection->BackUpOperUsed, 1, 0) == 0) {
            QUIC_OPERATION* Oper = &Connection->BackUpOper;
            Oper->FreeAfterProcess = FALSE;
            Oper->Type = QUIC_OPER_TYPE_API_CALL;
            Oper->API_CALL.Context = &Connection->BackupApiContext;
            Oper->API_CALL.Context->Type = QUIC_API_TYPE_CONN_SHUTDOWN;
            Oper->API_CALL.Context->CONN_SHUTDOWN.Flags = QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT;
            Oper->API_CALL.Context->CONN_SHUTDOWN.ErrorCode = 0;
#pragma prefast(suppress:6001, "SAL doesn't understand ref counts")
            QuicConnQueueOper(NewConnection, Oper);
        }

    } else {
        QuicConnRelease(NewConnection, QUIC_CONN_REF_HANDLE_OWNER);
    }

    return Connection;
}

//
// Looks up or creates a connection to handle a chain of datagrams.
// Returns TRUE if the datagrams were delivered, and FALSE if they should be
// dropped.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_DATAPATH_RECEIVE_CALLBACK)
BOOLEAN
QuicBindingDeliverDatagrams(
    _In_ QUIC_BINDING* Binding,
    _In_ QUIC_RECV_DATAGRAM* DatagramChain,
    _In_ uint32_t DatagramChainLength
    )
{
    QUIC_RECV_PACKET* Packet =
            QuicDataPathRecvDatagramToRecvPacket(DatagramChain);
    QUIC_DBG_ASSERT(Packet->ValidatedHeaderInv);

    //
    // For client owned bindings (for which we always control the CID) or for
    // short header packets for server owned bindings, the packet's destination
    // connection ID (DestCid) is the key for looking up the corresponding
    // connection object. The DestCid encodes the partition ID (PID) that can
    // be used for partitioning the look up table.
    //
    // For long header packets for server owned bindings, the packet's DestCid
    // was not necessarily generated locally, so cannot be used for routing.
    // Instead, a hash of the tuple and source connection ID (SourceCid) is

    // used.
    //
    // The exact type of lookup table associated with the binding varies on the
    // circumstances, but it allows for quick and easy lookup based on DestCid
    // (when used).
    //
    // If the lookup fails, and if there is a listener on the local 2-Tuple,
    // then a new connection is created and inserted into the binding's lookup
    // table.
    //
    // If a new connection is created, it will then be initially processed by
    // a library worker thread to decode the ALPN and SNI. That information
    // will then be used to find the associated listener. If not found, the
    // connection will be thrown away. Otherwise, the listener will then be
    // invoked to allow it to accept the connection and choose a server
    // certificate.
    //
    // If all else fails, and no connection was found or created for the
    // packet, then the packet is dropped.
    //

    QUIC_CONNECTION* Connection;
    if (!Binding->ServerOwned || Packet->IsShortHeader) {
        Connection =
            QuicLookupFindConnectionByLocalCid(
                &Binding->Lookup,
                Packet->DestCid,
                Packet->DestCidLen);
    } else {
        Connection =
            QuicLookupFindConnectionByRemoteHash(
                &Binding->Lookup,
                &DatagramChain->Tuple->RemoteAddress,
                Packet->SourceCidLen,
                Packet->SourceCid);
    }

    if (Connection == NULL) {

        //
        // Because the packet chain is ordered by control packets first, we
        // don't have to worry about a packet that can't create the connection
        // being in front of a packet that can in the chain. So we can always
        // use the head of the chain to determine if a new connection should
        // be created.
        //

        if (Binding->Exclusive) {
            QuicPacketLogDrop(Binding, Packet, "No connection on exclusive binding");
            return FALSE;
        }

        if (Packet->IsShortHeader) {
            //
            // For unattributed short header packets we can try to send a
            // stateless reset back in response.
            //
            return QuicBindingQueueStatelessReset(Binding, DatagramChain);
        }

        if (Packet->Invariant->LONG_HDR.Version == QUIC_VERSION_VER_NEG) {
            QuicPacketLogDrop(Binding, Packet, "Version negotiation packet not matched with a connection");
            return FALSE;
        }

        //
        // The following logic is server specific for creating/accepting new
        // connections.
        //

        QUIC_DBG_ASSERT(QuicIsVersionSupported(Packet->Invariant->LONG_HDR.Version));

        //
        // Only Initial (version specific) packets are processed from here on.
        //
        switch (Packet->Invariant->LONG_HDR.Version) {
        case QUIC_VERSION_DRAFT_27:
        case QUIC_VERSION_DRAFT_28:
        case QUIC_VERSION_DRAFT_29:
        case QUIC_VERSION_MS_1:
            if (Packet->LH->Type != QUIC_INITIAL) {
                QuicPacketLogDrop(Binding, Packet, "Non-initial packet not matched with a connection");
                return FALSE;
            }
        }

        const uint8_t* Token = NULL;
        uint16_t TokenLength = 0;
        if (!QuicPacketValidateLongHeaderV1(
                Binding,
                TRUE,
                Packet,
                &Token,
                &TokenLength)) {
            return FALSE;
        }

        QUIC_DBG_ASSERT(Token != NULL);

        if (!QuicBindingHasListenerRegistered(Binding)) {
            QuicPacketLogDrop(Binding, Packet, "No listeners registered to accept new connection.");
            return FALSE;
        }

        QUIC_DBG_ASSERT(Binding->ServerOwned);

        BOOLEAN DropPacket = FALSE;
        if (QuicBindingShouldRetryConnection(
                Binding, Packet, TokenLength, Token, &DropPacket)) {
            return
                QuicBindingQueueStatelessOperation(
                    Binding, QUIC_OPER_TYPE_RETRY, DatagramChain);

        } else if (!DropPacket) {
            Connection = QuicBindingCreateConnection(Binding, DatagramChain);
        }
    }

    if (Connection != NULL) {
        QuicConnQueueRecvDatagrams(Connection, DatagramChain, DatagramChainLength);
        QuicConnRelease(Connection, QUIC_CONN_REF_LOOKUP_RESULT);
        return TRUE;
    } else {
        return FALSE;
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_DATAPATH_RECEIVE_CALLBACK)
void
QuicBindingReceive(
    _In_ QUIC_DATAPATH_BINDING* DatapathBinding,
    _In_ void* RecvCallbackContext,
    _In_ QUIC_RECV_DATAGRAM* DatagramChain
    )
{
    UNREFERENCED_PARAMETER(DatapathBinding);
    QUIC_DBG_ASSERT(RecvCallbackContext != NULL);
    QUIC_DBG_ASSERT(DatagramChain != NULL);

    QUIC_BINDING* Binding = (QUIC_BINDING*)RecvCallbackContext;
    QUIC_RECV_DATAGRAM* ReleaseChain = NULL;
    QUIC_RECV_DATAGRAM** ReleaseChainTail = &ReleaseChain;
    QUIC_RECV_DATAGRAM* SubChain = NULL;
    QUIC_RECV_DATAGRAM** SubChainTail = &SubChain;
    QUIC_RECV_DATAGRAM** SubChainDataTail = &SubChain;
    uint32_t SubChainLength = 0;

    //
    // Breaks the chain of datagrams into subchains by destination CID and
    // delivers the subchains.
    //
    // NB: All packets in a datagram are required to have the same destination
    // CID, so we don't split datagrams here. Later on, the packet handling
    // code will check that each packet has a destination CID matching the
    // connection it was delivered to.
    //

    QUIC_RECV_DATAGRAM* Datagram;
    while ((Datagram = DatagramChain) != NULL) {

        //
        // Remove the head.
        //
        DatagramChain = Datagram->Next;
        Datagram->Next = NULL;

        QUIC_RECV_PACKET* Packet =
            QuicDataPathRecvDatagramToRecvPacket(Datagram);
        QuicZeroMemory(Packet, sizeof(QUIC_RECV_PACKET));
        Packet->Buffer = Datagram->Buffer;
        Packet->BufferLength = Datagram->BufferLength;

#if QUIC_TEST_DATAPATH_HOOKS_ENABLED
        //
        // The test datapath receive callback allows for test code to modify
        // the datagrams on the receive path, and optionally indicate one or
        // more to be dropped.
        //
        QUIC_TEST_DATAPATH_HOOKS* Hooks = MsQuicLib.TestDatapathHooks;
        if (Hooks != NULL) {
            if (Hooks->Receive(Datagram)) {
                *ReleaseChainTail = Datagram;
                ReleaseChainTail = &Datagram->Next;
                QuicPacketLogDrop(Binding, Packet, "Test Dopped");
                continue;
            }
        }
#endif

        //
        // Perform initial validation.
        //
        BOOLEAN ReleaseDatagram;
        if (!QuicBindingPreprocessDatagram(Binding, Datagram, &ReleaseDatagram)) {
            if (ReleaseDatagram) {
                *ReleaseChainTail = Datagram;
                ReleaseChainTail = &Datagram->Next;
            }
            continue;
        }

        QUIC_DBG_ASSERT(Packet->DestCid != NULL);
        QUIC_DBG_ASSERT(Packet->DestCidLen != 0 || Binding->Exclusive);
        QUIC_DBG_ASSERT(Packet->ValidatedHeaderInv);

        //
        // If the next datagram doesn't match the current subchain, deliver the
        // current subchain and start a new one.
        // (If the binding is exclusively owned, all datagrams are delivered to
        // the same connection and this chain-splitting step is skipped.)
        //
        QUIC_RECV_PACKET* SubChainPacket =
            SubChain == NULL ?
                NULL : QuicDataPathRecvDatagramToRecvPacket(SubChain);
        if (!Binding->Exclusive && SubChain != NULL &&
            (Packet->DestCidLen != SubChainPacket->DestCidLen ||
             memcmp(Packet->DestCid, SubChainPacket->DestCid, Packet->DestCidLen) != 0)) {
            if (!QuicBindingDeliverDatagrams(Binding, SubChain, SubChainLength)) {
                *ReleaseChainTail = SubChain;
                ReleaseChainTail = SubChainDataTail;
            }
            SubChain = NULL;
            SubChainTail = &SubChain;
            SubChainDataTail = &SubChain;
            SubChainLength = 0;
        }

        //
        // Insert the datagram into the current chain, with handshake packets
        // first (we assume handshake packets don't come after non-handshake
        // packets in a datagram).
        // We do this so that we can more easily determine if the chain of
        // packets can create a new connection.
        //

        SubChainLength++;
        if (!QuicPacketIsHandshake(Packet->Invariant)) {
            *SubChainDataTail = Datagram;
            SubChainDataTail = &Datagram->Next;
        } else {
            if (*SubChainTail == NULL) {
                *SubChainTail = Datagram;
                SubChainTail = &Datagram->Next;
                SubChainDataTail = &Datagram->Next;
            } else {
                Datagram->Next = *SubChainTail;
                *SubChainTail = Datagram;
                SubChainTail = &Datagram->Next;
            }
        }
    }

    if (SubChain != NULL) {
        //
        // Deliver the last subchain.
        //
        if (!QuicBindingDeliverDatagrams(Binding, SubChain, SubChainLength)) {
            *ReleaseChainTail = SubChain;
            ReleaseChainTail = SubChainTail;
        }
    }

    if (ReleaseChain != NULL) {
        QuicDataPathBindingReturnRecvDatagrams(ReleaseChain);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_DATAPATH_UNREACHABLE_CALLBACK)
void
QuicBindingUnreachable(
    _In_ QUIC_DATAPATH_BINDING* DatapathBinding,
    _In_ void* Context,
    _In_ const QUIC_ADDR* RemoteAddress
    )
{
    UNREFERENCED_PARAMETER(DatapathBinding);
    QUIC_DBG_ASSERT(Context != NULL);
    QUIC_DBG_ASSERT(RemoteAddress != NULL);

    QUIC_BINDING* Binding = (QUIC_BINDING*)Context;

    QUIC_CONNECTION* Connection =
        QuicLookupFindConnectionByRemoteAddr(
            &Binding->Lookup,
            RemoteAddress);

    if (Connection != NULL) {
        QuicConnQueueUnreachable(Connection, RemoteAddress);
        QuicConnRelease(Connection, QUIC_CONN_REF_LOOKUP_RESULT);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicBindingSendTo(
    _In_ QUIC_BINDING* Binding,
    _In_ const QUIC_ADDR * RemoteAddress,
    _In_ QUIC_DATAPATH_SEND_CONTEXT* SendContext
    )
{
    QUIC_STATUS Status;

#if QUIC_TEST_DATAPATH_HOOKS_ENABLED
    QUIC_TEST_DATAPATH_HOOKS* Hooks = MsQuicLib.TestDatapathHooks;
    if (Hooks != NULL) {

        QUIC_ADDR RemoteAddressCopy = *RemoteAddress;
        BOOLEAN Drop =
            Hooks->Send(
                &RemoteAddressCopy,
                NULL,
                SendContext);

        if (Drop) {
            QuicTraceLogVerbose(
                BindingSendToTestDrop,
                "[bind][%p] Test dropped packet",
                Binding);
            QuicDataPathBindingFreeSendContext(SendContext);
            Status = QUIC_STATUS_SUCCESS;
        } else {
            Status =
                QuicDataPathBindingSendTo(
                    Binding->DatapathBinding,
                    &RemoteAddressCopy,
                    SendContext);
            if (QUIC_FAILED(Status)) {
                QuicTraceLogWarning(
                    BindingSendToFailed,
                    "[bind][%p] SendTo failed, 0x%x",
                    Binding,
                    Status);
            }
        }
    } else {
#endif
        Status =
            QuicDataPathBindingSendTo(
                Binding->DatapathBinding,
                RemoteAddress,
                SendContext);
        if (QUIC_FAILED(Status)) {
            QuicTraceLogWarning(
                BindingSendToFailed,
                "[bind][%p] SendTo failed, 0x%x",
                Binding,
                Status);
        }
#if QUIC_TEST_DATAPATH_HOOKS_ENABLED
    }
#endif

    return Status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicBindingSendFromTo(
    _In_ QUIC_BINDING* Binding,
    _In_ const QUIC_ADDR * LocalAddress,
    _In_ const QUIC_ADDR * RemoteAddress,
    _In_ QUIC_DATAPATH_SEND_CONTEXT* SendContext
    )
{
    QUIC_STATUS Status;

#if QUIC_TEST_DATAPATH_HOOKS_ENABLED
    QUIC_TEST_DATAPATH_HOOKS* Hooks = MsQuicLib.TestDatapathHooks;
    if (Hooks != NULL) {

        QUIC_ADDR RemoteAddressCopy = *RemoteAddress;
        QUIC_ADDR LocalAddressCopy = *LocalAddress;
        BOOLEAN Drop =
            Hooks->Send(
                &RemoteAddressCopy,
                &LocalAddressCopy,
                SendContext);

        if (Drop) {
            QuicTraceLogVerbose(
                BindingSendFromToTestDrop,
                "[bind][%p] Test dropped packet",
                Binding);
            QuicDataPathBindingFreeSendContext(SendContext);
            Status = QUIC_STATUS_SUCCESS;
        } else {
            Status =
                QuicDataPathBindingSendFromTo(
                    Binding->DatapathBinding,
                    &LocalAddressCopy,
                    &RemoteAddressCopy,
                    SendContext);
            if (QUIC_FAILED(Status)) {
                QuicTraceLogWarning(
                    BindingSendFromToFailed,
                    "[bind][%p] SendFromTo failed, 0x%x",
                    Binding,
                    Status);
            }
        }
    } else {
#endif
        Status =
            QuicDataPathBindingSendFromTo(
                Binding->DatapathBinding,
                LocalAddress,
                RemoteAddress,
                SendContext);
        if (QUIC_FAILED(Status)) {
            QuicTraceLogWarning(
                BindingSendFromToFailed,
                "[bind][%p] SendFromTo failed, 0x%x",
                Binding,
                Status);
        }
#if QUIC_TEST_DATAPATH_HOOKS_ENABLED
    }
#endif

    return Status;
}

QUIC_STATIC_ASSERT(
    QUIC_HASH_SHA256_SIZE >= QUIC_STATELESS_RESET_TOKEN_LENGTH,
    "Stateless reset token must be shorter than hash size used");

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicBindingGenerateStatelessResetToken(
    _In_ QUIC_BINDING* Binding,
    _In_reads_(MsQuicLib.CidTotalLength)
        const uint8_t* const CID,
    _Out_writes_all_(QUIC_STATELESS_RESET_TOKEN_LENGTH)
        uint8_t* ResetToken
    )
{
    uint8_t HashOutput[QUIC_HASH_SHA256_SIZE];
    QuicDispatchLockAcquire(&Binding->ResetTokenLock);
    QUIC_STATUS Status =
        QuicHashCompute(
            Binding->ResetTokenHash,
            CID,
            MsQuicLib.CidTotalLength,
            sizeof(HashOutput),
            HashOutput);
    QuicDispatchLockRelease(&Binding->ResetTokenLock);
    if (QUIC_SUCCEEDED(Status)) {
        QuicCopyMemory(
            ResetToken,
            HashOutput,
            QUIC_STATELESS_RESET_TOKEN_LENGTH);
    }
    return Status;
}
