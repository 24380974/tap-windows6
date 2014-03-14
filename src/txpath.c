/*
 *  TAP-Windows -- A kernel driver to provide virtual tap
 *                 device functionality on Windows.
 *
 *  This code was inspired by the CIPE-Win32 driver by Damion K. Wilson.
 *
 *  This source code is Copyright (C) 2002-2014 OpenVPN Technologies, Inc.,
 *  and is released under the GPL version 2 (see below).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

//
// Include files.
//

#include "tap-windows.h"

//======================================================================
// TAP Send Path Support
//======================================================================

#ifdef ALLOC_PRAGMA
#pragma alloc_text( PAGE, TapDeviceWrite)
#endif // ALLOC_PRAGMA

VOID
tapAdapterTransmit(
    __in PTAP_ADAPTER_CONTEXT   Adapter,
    __in PNET_BUFFER            NetBuffer,
    __in  BOOLEAN               DispatchLevel
    )
/*++

Routine Description:

    This routine is called to transmit an individual net buffer using a
    style similar to the previous NDIS 5 AdapterTransmit function.

    In this implementation adapter state and NB length checks have already
    been done before this function has been called.

    The net buffer will be completed by the calling routine after this
    routine exits. So, under this design it is necessary to make a deep
    copy of frame data in the net buffer.

    This routine creates a flat buffer copy of NB frame data. This is an
    unnecessary performance bottleneck. However, the bottleneck is probably
    not significant or measurable except for adapters running at 1Gbps or
    greater speeds. Since this adapter is currently running at 100Mbps this
    defect can be ignored.

    Runs at IRQL <= DISPATCH_LEVEL

Arguments:

    Adapter                     Pointer to our adapter context
    NetBuffer                   Pointer to the net buffer to transmit
    DispatchLevel               TRUE if called at IRQL == DISPATCH_LEVEL

Return Value:

    None.

    In the Microsoft NDIS 6 architecture there is no per-packet status.

--*/
{
    NDIS_STATUS     status;
    ULONG           packetLength;
    PTAP_PACKET     tapPacket;
    PVOID           packetData;

    packetLength = NET_BUFFER_DATA_LENGTH(NetBuffer);

    // Allocate TAP packet memory
    tapPacket = (PTAP_PACKET )NdisAllocateMemoryWithTagPriority(
                    Adapter->MiniportAdapterHandle,
                    TAP_PACKET_SIZE (packetLength),
                    TAP_PACKET_TAG,
                    NormalPoolPriority
                    );

    if(tapPacket == NULL)
    {
        DEBUGP (("[TAP] tapAdapterTransmit: TAP packet allocation failed\n"));
        return;
    }

    tapPacket->m_SizeFlags = (packetLength & TP_SIZE_MASK);

    //
    // Reassemble packet contents
    // --------------------------
    // NdisGetDataBuffer does most of the work. There are two cases:
    //
    //    1.) If the NB data was not contiguous it will copy the entire
    //        NB's data to m_data and return pointer to m_data.
    //    2.) If the NB data was contiguous it returns a pointer to the
    //        first byte of the contiguous data instead of a pointer to m_Data.
    //        In this case the data will not have been copied to m_Data. Copy
    //        to m_Data will need to be done in an extra step.
    //
    // Case 1.) is the most likely in normal operation.
    //
    packetData = NdisGetDataBuffer(NetBuffer,packetLength,tapPacket->m_Data,1,0);

    if(packetData == NULL)
    {
        DEBUGP (("[TAP] tapAdapterTransmit: Could not get packet data\n"));

        NdisFreeMemory(tapPacket,0,0);

        return;
    }

    if(packetData != tapPacket->m_Data)
    {
        // Packet data was contiguous and not yet copied to m_Data.
        NdisMoveMemory(tapPacket->m_Data,packetData,packetLength);
    }
    
    DUMP_PACKET ("AdapterTransmit", tapPacket->m_Data, packetLength);

    //=====================================================
    // If IPv4 packet, check whether or not packet
    // was truncated.
    //=====================================================
#if PACKET_TRUNCATION_CHECK
    IPv4PacketSizeVerify (tapPacket->m_Data, packetLength, FALSE, "TX", &Adapter->m_TxTrunc);
#endif

/*
    //=====================================================
    // Are we running in DHCP server masquerade mode?
    //
    // If so, catch both DHCP requests and ARP queries
    // to resolve the address of our virtual DHCP server.
    //=====================================================
    if (Adapter->m_dhcp_enabled)
    {
        const ETH_HEADER *eth = (ETH_HEADER *) tapPacket->m_Data;
        const IPHDR *ip = (IPHDR *) (tapPacket->m_Data + sizeof (ETH_HEADER));
        const UDPHDR *udp = (UDPHDR *) (tapPacket->m_Data + sizeof (ETH_HEADER) + sizeof (IPHDR));

        // ARP packet?
        if (packetLength == sizeof (ARP_PACKET)
            && eth->proto == htons (ETH_P_ARP)
            && Adapter->m_dhcp_server_arp
            )
        {
            if (ProcessARP (Adapter,
                (PARP_PACKET) tapPacket->m_Data,
                Adapter->m_dhcp_addr,
                Adapter->m_dhcp_server_ip,
                ~0,
                Adapter->m_dhcp_server_mac))
                goto no_queue;
        }

        // DHCP packet?
        else if (packetLength >= sizeof (ETH_HEADER) + sizeof (IPHDR) + sizeof (UDPHDR) + sizeof (DHCP)
            && eth->proto == htons (ETH_P_IP)
            && ip->version_len == 0x45 // IPv4, 20 byte header
            && ip->protocol == IPPROTO_UDP
            && udp->dest == htons (BOOTPS_PORT)
            )
        {
            const DHCP *dhcp = (DHCP *) (tapPacket->m_Data
                + sizeof (ETH_HEADER)
                + sizeof (IPHDR)
                + sizeof (UDPHDR));

            const int optlen = packetLength
                - sizeof (ETH_HEADER)
                - sizeof (IPHDR)
                - sizeof (UDPHDR)
                - sizeof (DHCP);

            if (optlen > 0) // we must have at least one DHCP option
            {
                if (ProcessDHCP (Adapter, eth, ip, udp, dhcp, optlen))
                    goto no_queue;
            }
            else
                goto no_queue;
        }
    }

    //===============================================
    // In Point-To-Point mode, check to see whether
    // packet is ARP (handled) or IPv4 (sent to app).
    // IPv6 packets are inspected for neighbour discovery
    // (to be handled locally), and the rest is forwarded
    // all other protocols are dropped
    //===============================================
    if (Adapter->m_tun)
    {
        ETH_HEADER *e;

        e = (ETH_HEADER *) tapPacket->m_Data;

        switch (ntohs (e->proto))
        {
        case ETH_P_ARP:

            // Make sure that packet is the right size for ARP.
            if (packetLength != sizeof (ARP_PACKET))
                goto no_queue;

            ProcessARP (
                Adapter,
                (PARP_PACKET) tapPacket->m_Data,
                Adapter->m_localIP,
                Adapter->m_remoteNetwork,
                Adapter->m_remoteNetmask,
                Adapter->m_TapToUser.dest
                );

        default:
            goto no_queue;

        case ETH_P_IP:

            // Make sure that packet is large enough to be IPv4.
            if (packetLength < (ETHERNET_HEADER_SIZE + IP_HEADER_SIZE))
            {
                goto no_queue;
            }

            // Only accept directed packets, not broadcasts.
            if (memcmp (e, &Adapter->m_TapToUser, ETHERNET_HEADER_SIZE))
            {
                goto no_queue;
            }

            // Packet looks like IPv4, queue it. :-)
            tapPacket->m_SizeFlags |= TP_TUN;
            break;

        case ETH_P_IPV6:
            // Make sure that packet is large enough to be IPv6.
            if (packetLength < (ETHERNET_HEADER_SIZE + IPV6_HEADER_SIZE))
            {
                goto no_queue;
            }

            // Broadcasts and multicasts are handled specially
            // (to be implemented)

            // Neighbor discovery packets to fe80::8 are special
            // OpenVPN sets this next-hop to signal "handled by tapdrv"
            if ( HandleIPv6NeighborDiscovery(Adapter,tapPacket->m_Data) )
            {
                goto no_queue;
            }

            // Packet looks like IPv6, queue it. :-)
            tapPacket->m_SizeFlags |= TP_TUN;
        }
    }
*/

    //===============================================
    // Push packet onto queue to wait for read from
    // userspace.
    //===============================================
    if(tapAdapterReadAndWriteReady(Adapter))
    {
        tapPacketQueueInsertTail(&Adapter->SendPacketQueue,tapPacket);
    }
    else
    {
        //
        // Tragedy. All this work and the packet is of no use... 
        //
        NdisFreeMemory(tapPacket,0,0);
    }

    // Return after queuing or freeing TAP packet.
    return;

    // Free TAP packet without queuing.
no_queue:
    if(tapPacket != NULL )
    {
        NdisFreeMemory(tapPacket,0,0);
    }
  
exit_success:
    return;
}

ULONG
tapGetNetBufferFrameType(
    __in PNET_BUFFER       NetBuffer
    )
/*++

Routine Description:

    Reads the network frame's destination address to determine the type
    (broadcast, multicast, etc)

    Runs at IRQL <= DISPATCH_LEVEL.

Arguments:

    NetBuffer                 The NB to examine

Return Value:

    NDIS_PACKET_TYPE_BROADCAST
    NDIS_PACKET_TYPE_MULTICAST
    NDIS_PACKET_TYPE_DIRECTED

--*/
{
    PETH_HEADER ethernetHeader;

    ethernetHeader = (PETH_HEADER )NdisGetDataBuffer(
                        NetBuffer,
                        sizeof(ETH_HEADER),
                        NULL,
                        1,
                        0
                        );

    ASSERT(ethernetHeader);

    if (ETH_IS_BROADCAST(ethernetHeader->dest))
    {
        return NDIS_PACKET_TYPE_BROADCAST;
    }
    else if(ETH_IS_MULTICAST(ethernetHeader->dest))
    {
        return NDIS_PACKET_TYPE_MULTICAST;
    }
    else
    {
        return NDIS_PACKET_TYPE_DIRECTED;
    }

}

ULONG
tapGetNetBufferCountsFromNetBufferList(
    __in PNET_BUFFER_LIST   NetBufferList,
    __inout_opt PULONG      TotalByteCount      // Of all linked NBs
    )
/*++

Routine Description:

    Returns the number of net buffers linked to the net buffer list.

    Optionally retuens the total byte count of all net buffers linked
    to the net buffer list

    Runs at IRQL <= DISPATCH_LEVEL.

Arguments:

    NetBufferList                 The NBL to examine

Return Value:

    The number of net buffers linked to the net buffer list.

--*/
{
    ULONG       netBufferCount = 0;
    PNET_BUFFER currentNb;

    if(TotalByteCount)
    {
        *TotalByteCount = 0;
    }

    currentNb = NET_BUFFER_LIST_FIRST_NB(NetBufferList);

    while(currentNb)
    {
        ++netBufferCount;

        if(TotalByteCount)
        {
            *TotalByteCount += NET_BUFFER_DATA_LENGTH(currentNb);
        }

        // Move to next NB
        currentNb = NET_BUFFER_NEXT_NB(currentNb);
    }

    return netBufferCount;
}

VOID
tapSendNetBufferListsComplete(
    __in PTAP_ADAPTER_CONTEXT   Adapter,
    __in PNET_BUFFER_LIST       NetBufferLists,
    __in NDIS_STATUS            SendCompletionStatus,
    __in BOOLEAN                DispatchLevel
    )
{
    PNET_BUFFER_LIST    currentNbl;
    PNET_BUFFER_LIST    nextNbl = NULL;
    ULONG               sendCompleteFlags = 0;

    for (
        currentNbl = NetBufferLists;
        currentNbl != NULL;
        currentNbl = nextNbl
        )
    {
        ULONG       frameType;
        ULONG       netBufferCount;
        ULONG       byteCount;

        nextNbl = NET_BUFFER_LIST_NEXT_NBL(currentNbl);

        // Set NBL completion status.
        NET_BUFFER_LIST_STATUS(currentNbl) = SendCompletionStatus;

        // Fetch first NBs frame type. All linked NBs will have same type.
        frameType = tapGetNetBufferFrameType(NET_BUFFER_LIST_FIRST_NB(currentNbl));

        // Fetch statistics for all NBs linked to the NB.
        netBufferCount = tapGetNetBufferCountsFromNetBufferList(
                            currentNbl,
                            &byteCount
                            );

        // Update statistics by frame type
        if(SendCompletionStatus == NDIS_STATUS_SUCCESS)
        {
            switch(frameType)
            {
            case NDIS_PACKET_TYPE_DIRECTED:
                Adapter->FramesTxDirected += netBufferCount;
                Adapter->BytesTxDirected += byteCount;
                break;

            case NDIS_PACKET_TYPE_BROADCAST:
                Adapter->FramesTxBroadcast += netBufferCount;
                Adapter->BytesTxBroadcast += byteCount;
                break;

            case NDIS_PACKET_TYPE_MULTICAST:
                Adapter->FramesTxMulticast += netBufferCount;
                Adapter->BytesTxMulticast += byteCount;
                break;

            default:
                ASSERT(FALSE);
                break;
            }
        }
        else
        {
            // Transmit error.
            Adapter->TransmitFailuresOther += netBufferCount;
        }

        currentNbl = nextNbl;
    }

    if(DispatchLevel)
    {
        sendCompleteFlags |= NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL;
    }

    // Complete the NBLs
    NdisMSendNetBufferListsComplete(
        Adapter->MiniportAdapterHandle,
        NetBufferLists,
        sendCompleteFlags
        );
}

BOOLEAN
tapNetBufferListNetBufferLengthsValid(
    __in PTAP_ADAPTER_CONTEXT   Adapter,
    __in  PNET_BUFFER_LIST      NetBufferLists
    )
/*++

Routine Description:

    Scan all NBLs and their linked NBs for valid lengths.

    Fairly absurd to find and packets with bogus lengths, but wise
    to check anyway. If ANY packet has a bogus length, then abort the
    entire send.

    The only time that one might see this check fail might be during
    HCK driver testing. The HKC test might send oversize packets to
    determine if the miniport can gracefully deal with them.

    This check is fairly fast. Unlike NDIS 5 packets, fetching NDIS 6
    packets lengths do not require any computation.

Arguments:

    Adapter                 Pointer to our adapter context
    NetBufferLists          Head of a list of NBLs to examine

Return Value:

    Returns TRUE if all NBs have reasonable lengths.
    Otherwise, returns FALSE.

--*/
{
    PNET_BUFFER_LIST        currentNbl;

    currentNbl = NetBufferLists;

    while (currentNbl)
    {
        PNET_BUFFER_LIST    nextNbl;
        PNET_BUFFER         currentNb;

        // Locate next NBL
        nextNbl = NET_BUFFER_LIST_NEXT_NBL(currentNbl);

        // Locate first NB (aka "packet")
        currentNb = NET_BUFFER_LIST_FIRST_NB(currentNbl);

        //
        // Process all NBs linked to this NBL
        //
        while(currentNb)
        {
            PNET_BUFFER nextNb;
            ULONG       packetLength;

            // Locate next NB
            nextNb = NET_BUFFER_NEXT_NB(currentNb);

            packetLength = NET_BUFFER_DATA_LENGTH(currentNb);

            // Minimum packet size is size of Ethernet plus IPv4 headers.
            ASSERT(packetLength >= (ETHERNET_HEADER_SIZE + IP_HEADER_SIZE));

            if(packetLength < (ETHERNET_HEADER_SIZE + IP_HEADER_SIZE))
            {
                return FALSE;
            }

            // Maximum size should be Ethernet header size plus MTU plus modest pad for
            // VLAN tag.
            ASSERT( packetLength <= (ETHERNET_HEADER_SIZE + VLAN_TAG_SIZE + Adapter->MtuSize));

            if(packetLength > (ETHERNET_HEADER_SIZE + VLAN_TAG_SIZE + Adapter->MtuSize))
            {
                return FALSE;
            }

            // Move to next NB
            currentNb = nextNb;
        }

        // Move to next NBL
        currentNbl = nextNbl;
    }

    return TRUE;
}

VOID
AdapterSendNetBufferLists(
    __in  NDIS_HANDLE             MiniportAdapterContext,
    __in  PNET_BUFFER_LIST        NetBufferLists,
    __in  NDIS_PORT_NUMBER        PortNumber,
    __in  ULONG                   SendFlags
    )
/*++

Routine Description:

    Send Packet Array handler. Called by NDIS whenever a protocol
    bound to our miniport sends one or more packets.

    The input packet descriptor pointers have been ordered according
    to the order in which the packets should be sent over the network
    by the protocol driver that set up the packet array. The NDIS
    library preserves the protocol-determined ordering when it submits
    each packet array to MiniportSendPackets

    As a deserialized driver, we are responsible for holding incoming send
    packets in our internal queue until they can be transmitted over the
    network and for preserving the protocol-determined ordering of packet
    descriptors incoming to its MiniportSendPackets function.
    A deserialized miniport driver must complete each incoming send packet
    with NdisMSendComplete, and it cannot call NdisMSendResourcesAvailable.

    Runs at IRQL <= DISPATCH_LEVEL

Arguments:

    MiniportAdapterContext      Pointer to our adapter
    NetBufferLists              Head of a list of NBLs to send
    PortNumber                  A miniport adapter port.  Default is 0.
    SendFlags                   Additional flags for the send operation

Return Value:

    None.  Write status directly into each NBL with the NET_BUFFER_LIST_STATUS
    macro.

--*/
{
    NDIS_STATUS             status;
    PTAP_ADAPTER_CONTEXT    adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;
    BOOLEAN                 DispatchLevel = (SendFlags & NDIS_SEND_FLAGS_DISPATCH_LEVEL);
    PNET_BUFFER_LIST        currentNbl;
    BOOLEAN                 validNbLengths;

    UNREFERENCED_PARAMETER(NetBufferLists);
    UNREFERENCED_PARAMETER(PortNumber);
    UNREFERENCED_PARAMETER(SendFlags);

    ASSERT(PortNumber == 0); // Only the default port is supported

    //
    // Can't process sends if TAP device is not open.
    // ----------------------------------------------
    // Just perform a "lying send" and return packets as if they
    // were successfully sent.
    //
    if(adapter->TapFileObject == NULL)
    {
        //
        // Complete all NBLs and return if adapter not ready.
        //
        tapSendNetBufferListsComplete(
            adapter,
            NetBufferLists,
            NDIS_STATUS_SUCCESS,
            DispatchLevel
            );

        return;
    }

    //
    // Check Adapter send/receive ready state.
    //
    status = tapAdapterSendAndReceiveReady(adapter);

    if(status != NDIS_STATUS_SUCCESS)
    {
        //
        // Complete all NBLs and return if adapter not ready.
        //
        tapSendNetBufferListsComplete(
            adapter,
            NetBufferLists,
            status,
            DispatchLevel
            );

        return;
    }

    //
    // Scan all NBLs and linked packets for valid lengths.
    // ---------------------------------------------------
    // If _ANY_ NB length is invalid, then fail the entire send operation.
    //
    //    BUGBUG!!! Perhaps this should be less agressive. Fail only individual
    //    NBLs...
    //    
    // If length check is valid, then TAP_PACKETS can be safely allocated
    // and processed for all NBs being sent.
    //
    validNbLengths = tapNetBufferListNetBufferLengthsValid(
                        adapter,
                        NetBufferLists
                        );

    if(!validNbLengths)
    {
        //
        // Complete all NBLs and return if and NB length is invalid.
        //
        tapSendNetBufferListsComplete(
            adapter,
            NetBufferLists,
            NDIS_STATUS_INVALID_LENGTH,
            DispatchLevel
            );

        return;
    }

    //
    // Process each NBL individually
    //
    currentNbl = NetBufferLists;

    while (currentNbl)
    {
        PNET_BUFFER_LIST    nextNbl;
        PNET_BUFFER         currentNb;

        // Locate next NBL
        nextNbl = NET_BUFFER_LIST_NEXT_NBL(currentNbl);

        // Locate first NB (aka "packet")
        currentNb = NET_BUFFER_LIST_FIRST_NB(currentNbl);

        // Transmit all NBs linked to this NBL
        while(currentNb)
        {
            PNET_BUFFER nextNb;

            // Locate next NB
            nextNb = NET_BUFFER_NEXT_NB(currentNb);

            // Transmit the NB
            tapAdapterTransmit(adapter,currentNb,DispatchLevel);

            // Move to next NB
            currentNb = nextNb;
        }

        // Move to next NBL
        currentNbl = nextNbl;
    }

    // Complete all NBLs
    tapSendNetBufferListsComplete(
        adapter,
        NetBufferLists,
        NDIS_STATUS_SUCCESS,
        DispatchLevel
        );
}

VOID
AdapterCancelSend(
    __in  NDIS_HANDLE             MiniportAdapterContext,
    __in  PVOID                   CancelId
    )
{
    PTAP_ADAPTER_CONTEXT   adapter = (PTAP_ADAPTER_CONTEXT )MiniportAdapterContext;

    //
    // This miniport completes its sends quickly, so it isn't strictly
    // neccessary to implement MiniportCancelSend.
    //
    // If we did implement it, we'd have to walk the Adapter->SendWaitList
    // and look for any NB that points to a NBL where the CancelId matches
    // NDIS_GET_NET_BUFFER_LIST_CANCEL_ID(Nbl).  For any NB that so matches,
    // we'd remove the NB from the SendWaitList and set the NBL's status to
    // NDIS_STATUS_SEND_ABORTED, then complete the NBL.
    //
}

// IRP_MJ_READ callback.
NTSTATUS
TapDeviceRead(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    NTSTATUS                ntStatus = STATUS_SUCCESS;// Assume success
    PIO_STACK_LOCATION      irpSp;// Pointer to current stack location
    PTAP_ADAPTER_CONTEXT    adapter = NULL;

    PAGED_CODE();

    irpSp = IoGetCurrentIrpStackLocation( Irp );

    //
    // Fetch adapter context for this device.
    // --------------------------------------
    // Adapter pointer was stashed in FsContext when handle was opened.
    //
    adapter = (PTAP_ADAPTER_CONTEXT )(irpSp->FileObject)->FsContext;

    ASSERT(adapter);

    //
    // Sanity checks on state variables
    //
    if (!tapAdapterReadAndWriteReady(adapter))
    {
        DEBUGP (("[%s] Interface is down in IRP_MJ_READ\n",
            MINIPORT_INSTANCE_ID (adapter)));

        NOTE_ERROR();
        Irp->IoStatus.Status = ntStatus = STATUS_UNSUCCESSFUL;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);

        return ntStatus;
    }

    // Save IRP-accessible copy of buffer length
    Irp->IoStatus.Information = irpSp->Parameters.Read.Length;

    if (Irp->MdlAddress == NULL)
    {
        DEBUGP (("[%s] MdlAddress is NULL for IRP_MJ_READ\n",
            MINIPORT_INSTANCE_ID (adapter)));

        NOTE_ERROR();
        Irp->IoStatus.Status = ntStatus = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);

        return ntStatus;
    }

    if ((Irp->AssociatedIrp.SystemBuffer
            = MmGetSystemAddressForMdlSafe(
                Irp->MdlAddress,
                NormalPagePriority
                ) ) == NULL
        )
    {
        DEBUGP (("[%s] Could not map address in IRP_MJ_READ\n",
            MINIPORT_INSTANCE_ID (adapter)));

        NOTE_ERROR();
        Irp->IoStatus.Status = ntStatus = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);

        return ntStatus;
    }

    // BUGBUG!!! Use RemoveLock???

    // BUGBUG!!! Service IRP immediately??? Queue if unable to do so.

    //
    // Queue the IRP and return STATUS_PENDING.
    // ----------------------------------------
    // Note: IoCsqInsertIrp marks the IRP pending.
    //

    // BUGBUG!!! NDIS 5 implementation has IRP_QUEUE_SIZE of 16 and 
    // does not queue IRP if this capacity is exceeded.
    //
    // Is this needed???
    //
    IoCsqInsertIrp(&adapter->PendingReadIrpQueue.CsqQueue, Irp, NULL);

    ntStatus = STATUS_PENDING;

    return ntStatus;
}

