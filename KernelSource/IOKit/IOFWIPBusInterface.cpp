#include "../../KernelHeaders/IOKit/IOFWIPBusInterface.h"
#include <IOKit/firewire/IOFireWireController.h>

#define BCOPY(s, d, l) do { bcopy((void *) s, (void *) d, l); } while(0)

struct GASPVAL
{
   UCHAR specifierID[3];      // 24-bit RID
   UCHAR version[3];          // 24-bit version
} gaspVal = { {0x00, 0x00, 0x5E}, {0x00, 0x00, 0x01} };

extern "C"
{
/*!
	@function watchdog
	@abstract watchdog timer - cleans the Link control block's rcb's.
	@param timer - IOTimerEventsource.
	@result void.
*/
void watchdog(OSObject *, IOTimerEventSource *);
}

#define super IOService

OSDefineMetaClassAndStructors(IOFWIPBusInterface, IOService)

OSDefineMetaClassAndStructors(MARB, OSObject)
OSDefineMetaClassAndStructors(ARB, OSObject)
OSDefineMetaClassAndStructors(DRB, OSObject)
OSDefineMetaClassAndStructors(RCB, IOCommand)
OSDefineMetaClassAndStructors(IOFWIPMBufCommand, IOCommand)


bool IOFWIPBusInterface::init(IOFireWireIP *primaryInterface)
{
    fIPLocalNode = OSDynamicCast(IOFireWireIP, primaryInterface);

    if(not fIPLocalNode)
        return false;

    fControl = fIPLocalNode->getController(); 

	if( not fControl)
		return false;

    if ( not super::init() )
        return false;

    fIP1394AddressSpace		= 0;
    fAsyncCmdPool			= 0;
	fMbufCmdPool			= 0;
	fRCBCmdPool				= 0;
    fAsyncStreamTxCmdPool	= 0;
	fAsyncTransitSet		= 0;
	fAsyncStreamTransitSet	= 0;
	fCurrentMBufCommands		= 0;
	fCurrentAsyncIPCommands		= 0;
	fCurrentRCBCommands			= 0;
	
	fLowWaterMark					= kLowWaterMark;
	fIPLocalNode->fMaxQueueSize		= TRANSMIT_QUEUE_SIZE;
	
	// set the secondary interface handlers with IOFireWireIP
	if( not attachIOFireWireIP ( fIPLocalNode ) )
		return false;

	fControl->retain();

	fIPLocalNode->retain();
	
	fControl->resetBus();

	fStarted  = true;
	
	return true;
}

bool IOFWIPBusInterface::finalize(IOOptionBits options)
{
	IORecursiveLockLock(fIPLock);

	fStarted = false;

	fIPLocalNode->deRegisterFWIPPrivateHandlers();
	
	// Release the Asyncstream receive broadcast client
	if(fBroadcastReceiveClient != NULL)
		fBroadcastReceiveClient->release();
	
	fBroadcastReceiveClient = NULL;

    if (fIP1394AddressSpace != NULL)
	{
        fIP1394AddressSpace->deactivate();
        fIP1394AddressSpace->release();
    }
	fIP1394AddressSpace = NULL;

	if(timerSource != NULL) 
	{
		if (workLoop != NULL)
			workLoop->removeEventSource(timerSource);
		timerSource->release();
	}
	timerSource = NULL;

	IORecursiveLockUnlock(fIPLock);

	IOFWIPAsyncWriteCommand *cmd1 = NULL;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( fAsyncTransitSet );
	while( NULL != (cmd1 = OSDynamicCast(IOFWIPAsyncWriteCommand, iterator->getNextObject())) )
	{
		if(cmd1->Busy())
			cmd1->wait();
	}
	iterator->release();
	fAsyncTransitSet->flushCollection();
	fAsyncTransitSet->free();
	fAsyncTransitSet = NULL;
	
	IOFWIPAsyncStreamTxCommand *cmd2 = NULL;
	iterator = OSCollectionIterator::withCollection( fAsyncStreamTransitSet );
	while( NULL != (cmd2 = OSDynamicCast(IOFWIPAsyncStreamTxCommand, iterator->getNextObject())) )
	{	
		if(cmd2->Busy())
			cmd2->wait();
	}
	iterator->release();
	fAsyncStreamTransitSet->flushCollection();
	fAsyncStreamTransitSet->free();
	fAsyncStreamTransitSet = NULL;

	return super::finalize(options);
}

void IOFWIPBusInterface::free()
{
	detachIOFireWireIP();

	super::free();
}

IOReturn IOFWIPBusInterface::message(UInt32 type, IOService *provider, void *argument)
{
    IOReturn  res = kIOReturnSuccess;

    switch (type)
    {                
        case kIOMessageServiceIsTerminated:
            break;

        case kIOMessageServiceIsSuspended:
            if(fStarted == true)
                stopReceivingBroadcast();
            break;

        case kIOMessageServiceIsResumed:
            if(fStarted == true)
            {
				resetRCBCache();

				resetMcapState();
				
				updateBroadcastValues(true);

                startReceivingBroadcast(fLcb->maxBroadcastSpeed);
            }
            break;
            
        case kIOMessageServiceIsRequestingClose:
            break;
            
        default: 
			res = kIOReturnUnsupported;
            break;
    }

    return res;
}



bool IOFWIPBusInterface::attachIOFireWireIP(IOFireWireIP *provider)
{
	fIPLocalNode	= provider;
	fLcb			= fIPLocalNode->getLcb();
	fIPLock			= fIPLocalNode->getIPLock();
	workLoop		= fIPLocalNode->getWorkLoop();

	if( initAsyncCmdPool() != kIOReturnSuccess)
		return false;

	unicastArb		= OSSet::withCapacity(kUnicastArbs);
	if(unicastArb == 0)
		return false;
		
	multicastArb 	= OSSet::withCapacity(kMulticastArbs);
	if(multicastArb == 0)
		return false;
		
	activeDrb		= OSSet::withCapacity(kActiveDrbs);
	if(activeDrb == 0)
		return false;
		
	activeRcb		= OSSet::withCapacity(kActiveRcbs);
	if(activeRcb == 0)
		return false;

	mcapState		= OSDictionary::withCapacity(kMaxChannels);
	if(mcapState == 0)
		return false;

	fAsyncStreamTransitSet = OSSet::withCapacity(kMaxAsyncStreamCommands);
	if(fAsyncStreamTransitSet == 0)
		return false;
	
	fAsyncTransitSet = OSSet::withCapacity(kMaxAsyncCommands);
	if(fAsyncTransitSet == 0)
		return false;
			
	// Does calculate the fMaxTxAsyncDoubleBuffer & fAsyncRxIsocPacketSize;
    calculateMaxTransferUnit();

	// Allocate Timer event source
	timerSource = IOTimerEventSource::timerEventSource ( ( OSObject* ) this,
													   ( IOTimerEventSource::Action ) &watchdog);
	if ( timerSource == NULL )
	{
		IOLog( "IOFireWireIP::start - Couldn't allocate timer event source\n" );
		return false;
	}

	if ( workLoop->addEventSource ( timerSource ) != kIOReturnSuccess )
	{
		IOLog( "IOFireWireIP::start - Couldn't add timer event source\n" );        
		return false;
	}

	// Asyncstream hook up to recieve the broadcast packets
	fBroadcastReceiveClient = new IOFWAsyncStreamRxCommand;
	if ( not fBroadcastReceiveClient )
		return false;
	
	if ( not fBroadcastReceiveClient->initAll(0x1f, rxAsyncStream, fControl, fMaxRxIsocPacketSize, this ) ) 
		return false;
	
	// Create pseudo address space
	if ( createIPFifoAddress (MAX_FIFO_SIZE) != kIOReturnSuccess)
		return false;

	// might eventually start the timer
	timerSource->setTimeoutMS ( WATCHDOG_TIMER_MS );
	
	IOFireWireIPPrivateHandlers privateHandlers;
	
	privateHandlers.newService = this;
    privateHandlers.transmitPacket = getOutputHandler();
	privateHandlers.updateARPCache = getARPCacheHandler();
	
	fIPLocalNode->registerFWIPPrivateHandlers(&privateHandlers);
	
	attach(fIPLocalNode);
	
	return true;
}

void IOFWIPBusInterface::detachIOFireWireIP()
{
	IORecursiveLockLock(fIPLock);
	
    freeAsyncCmdPool();
	
	freeAsyncStreamCmdPool();

    if(fMbufCmdPool != NULL)
	{
		IOFWIPMBufCommand *cmd = NULL;
		do
		{
			cmd = (IOFWIPMBufCommand*)fMbufCmdPool->getCommand(false);
			if(cmd != NULL)
				cmd->release();
		}while(cmd != NULL);
		
		fMbufCmdPool->release();
		fMbufCmdPool = NULL;
		fCurrentMBufCommands = 0;
	}

    if(fRCBCmdPool != NULL)
	{
		RCB *cmd = NULL;
		do
		{
			cmd = (RCB*)fRCBCmdPool->getCommand(false);
			if(cmd != NULL)
				cmd->release();
		}while(cmd != NULL);
		
		fRCBCmdPool->release();
		fRCBCmdPool = NULL;
		fCurrentRCBCommands = 0;
	}
	
	if(unicastArb != NULL)
	{
		{
			ARB *arb = 0;
			OSCollectionIterator * iterator = OSCollectionIterator::withCollection( unicastArb );

			while( NULL != (arb = OSDynamicCast(ARB, iterator->getNextObject())) )
			{
				unicastArb->removeObject(arb);
				arb->release();
			}
			
			iterator->release();
		}
		unicastArb->flushCollection();
		unicastArb->free();
		unicastArb = NULL;
	}

	if(multicastArb != NULL)
	{
		{
			MARB *marb = 0;
			OSCollectionIterator * iterator = OSCollectionIterator::withCollection( multicastArb );

			while( NULL != (marb = OSDynamicCast(MARB, iterator->getNextObject())) )
			{
				multicastArb->removeObject(marb);
				marb->release();
			}
			
			iterator->release();
		}

		multicastArb->flushCollection();
		multicastArb->free();
		multicastArb = NULL;
	}
	
	if(activeDrb != NULL)
	{
		{
			DRB *drb = 0;
			OSCollectionIterator * iterator = OSCollectionIterator::withCollection( activeDrb );

			while( NULL != (drb = OSDynamicCast(DRB, iterator->getNextObject())) )
			{
				activeDrb->removeObject(drb);
				drb->release();
			}
			
			iterator->release();
		}
	
		activeDrb->flushCollection();
		activeDrb->free();
		activeDrb = NULL;
	}
	
	if(activeRcb != NULL)
	{
		{
			RCB *rcb = 0;
			OSCollectionIterator * iterator = OSCollectionIterator::withCollection( activeRcb );

			while( NULL != (rcb = OSDynamicCast(RCB, iterator->getNextObject())) )
			{
				activeRcb->removeObject(rcb);
				rcb->release();
			}
			
			iterator->release();
		}
	
		activeRcb->flushCollection();
		activeRcb->free();
		activeRcb = NULL;
	}
	
	if(mcapState != NULL)
	{
		mcapState->flushCollection();
		mcapState->free();
		mcapState = NULL;
	}
		
	if(fControl)
		fControl->release();
		
	fControl = NULL;

	IORecursiveLockUnlock(fIPLock);
	
	if(fIPLocalNode)
	{
		detach(fIPLocalNode);
		fIPLocalNode->release();
	}
		
	fIPLocalNode = NULL;
}

void IOFWIPBusInterface::decrementUnitCount()
{
	recursiveScopeLock lock(fIPLock);

	if(fUnitCount > 0)
		fUnitCount--;
}

void IOFWIPBusInterface::incrementUnitCount()
{
	recursiveScopeLock lock(fIPLock);
	
	fUnitCount++;
}

SInt16 IOFWIPBusInterface::getUnitCount()
{
	recursiveScopeLock lock(fIPLock);

	return fUnitCount;
}

/*!
	@function fwIPUnitAttach
	@abstract Callback for a Unit Attach of type IPv4 or IPv6
	@result void.
*/	
void IOFWIPBusInterface::fwIPUnitAttach()
{
	incrementUnitCount();
	
	updateBroadcastValues(true);
	
	fLowWaterMark = kLowWaterMark; // new unit, so lets learn afresh
}

/*!
	@function fwIPUnitTerminate
	@abstract Callback for a Unit detach of type IP1394
	@result void.
*/	
void IOFWIPBusInterface::fwIPUnitTerminate()
{
	decrementUnitCount();
	
	updateBroadcastValues(true);
}

/*!
	@function updateBroadcastValues
	@abstract Updates the max broadcast payload and speed  
	@param reset - useful to know whether to start from beginning.
	@result void.
*/	
void IOFWIPBusInterface::updateBroadcastValues(bool reset)
{
	IOFireWireDevice *remoteDevice = NULL;

	IORecursiveLockLock(fIPLock);
	
	if(reset)
	{
		IOFireWireDevice *localDevice = (IOFireWireDevice*)(fIPLocalNode->getDevice());
		
		fLcb->maxBroadcastPayload = localDevice->maxPackLog(true);

		fLcb->maxBroadcastSpeed = localDevice->FWSpeed();
		// Update our own max payload
		fLcb->ownMaxPayload = localDevice->maxPackLog(true);
		// Update the nodeID
		localDevice->getNodeIDGeneration(fLcb->busGeneration, fLcb->ownNodeID);
		// Update the speed
		fLcb->ownMaxSpeed = localDevice->FWSpeed();
	}

	// Display the active DRB
	DRB *drb = NULL;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( activeDrb );
	while( NULL != (drb = OSDynamicCast(DRB, iterator->getNextObject())) )
	{ 
		remoteDevice = (IOFireWireDevice*)drb->deviceID;
			
		if(fLcb->maxBroadcastSpeed > drb->maxSpeed)
			fLcb->maxBroadcastSpeed = drb->maxSpeed;
		
		if(fLcb->maxBroadcastPayload > drb->maxPayload)
			fLcb->maxBroadcastPayload = drb->maxPayload;
	}
   	iterator->release();

	updateLinkStatus();

    IORecursiveLockUnlock(fIPLock);
}

/*!
	@function updateLinkStatus
	@abstract Updates the link status based on maxbroadcast speed & payload.  
	@param None.
	@result void.
*/	
void IOFWIPBusInterface::updateLinkStatus()
{
	IORecursiveLockLock(fIPLock);

	// set medium inactive, before setting it to active for radar 3300357
	fIPLocalNode->setLinkStatus(kIONetworkLinkValid, fIPLocalNode->getCurrentMedium(), 0); 

	// lets update the link status as active, if only units are greater than 0
	if(fUnitCount > 0)
		fIPLocalNode->setLinkStatus (kIONetworkLinkActive | kIONetworkLinkValid,
						fIPLocalNode->getCurrentMedium(), 
						(1 << fLcb->maxBroadcastSpeed) * 100 * 1000000);
	
	fLcb->ownHardwareAddress.spd = fLcb->maxBroadcastSpeed;
	// fix to enable the arp/dhcp support from network pref pane
	fIPLocalNode->setProperty(kIOFWHWAddr,  (void *)&fLcb->ownHardwareAddress, sizeof(IP1394_HDW_ADDR));
	
	IORecursiveLockUnlock(fIPLock);
}

/*!
	@function createIPFifoAddress
	@abstract creates the pseudo address space for IP over Firewire.
	@param 	UInt32 fifosize - size of the pseudo address space
	@result IOReturn - kIOReturnSuccess or error if failure.
*/
IOReturn IOFWIPBusInterface::createIPFifoAddress(UInt32 fifosize)
{
    IOReturn		ioStat = kIOReturnSuccess;

	// add  csr address space
    fIP1394AddressSpace = fControl->createPseudoAddressSpace(&fIP1394Address, fifosize,
                                                            NULL,
                                                            &rxUnicast,
                                                            this);
    if (fIP1394AddressSpace == NULL)
        ioStat = kIOReturnNoMemory;

    if(ioStat == kIOReturnSuccess )		// change for performance, coalescing incoming writes
		fIP1394AddressSpace->setARxReqIntCompleteHandler(this, &rxUnicastComplete);

	fLcb->ownHardwareAddress.unicastFifoHi = fIP1394Address.addressHi;      
	fLcb->ownHardwareAddress.unicastFifoLo = fIP1394Address.addressLo;
	// fix to enable the arp/dhcp support from network pref pane
	fIPLocalNode->setProperty(kIOFWHWAddr,  (void *)&fLcb->ownHardwareAddress, sizeof(IP1394_HDW_ADDR));

    if(ioStat == kIOReturnSuccess )
        ioStat = fIP1394AddressSpace->activate();
    
    if(ioStat != kIOReturnSuccess)
        IOLog("IOFireWireIP PseudoAddressSpace Activate failure status %d\n", ioStat);

    // end of csr address space
	return ioStat;
}

/*!
	@function calculateMaxTransferUnit
	@abstract checks whether the FWIM is for calculateMaxTransferUnit H/W, if not 
			  sets appropriate performance related params
	@param none.
	@result Returns void.
*/
void IOFWIPBusInterface::calculateMaxTransferUnit()
{
    IORegistryEntry			*parent = fControl->getParentEntry(gIOServicePlane); 

	if(strcmp(parent->getName(gIOServicePlane), "AppleLynx") == 0)
	{
		fMaxRxIsocPacketSize	= 2048;
		fMaxTxAsyncDoubleBuffer =  1 << 9;
	} 
	else
	{
		fMaxRxIsocPacketSize	= 4096;
		fMaxTxAsyncDoubleBuffer = 1 << ((IOFireWireNub*)(fIPLocalNode->getDevice()))->maxPackLog(true);
	}
	
	fIPLocalNode->updateMTU(fMaxTxAsyncDoubleBuffer);
}

/*!
	@function initAsyncCmdPool
	@abstract constructs Asynchronous command objects and queues them in the pool
	@param none.
	@result Returns kIOReturnSuccess if it was successful, else kIOReturnNoMemory.
*/
UInt32 IOFWIPBusInterface::initAsyncCmdPool()
{
    IOReturn status = kIOReturnSuccess;

	if( (fAsyncCmdPool == NULL) )
		fAsyncCmdPool = IOCommandPool::withWorkLoop(workLoop);
	
	if( (fMbufCmdPool == NULL) )
		fMbufCmdPool = IOCommandPool::withWorkLoop(workLoop);

	if( (fRCBCmdPool == NULL) )
		fRCBCmdPool = IOCommandPool::withWorkLoop(workLoop);
	
	if( (fMbufCmdPool == NULL) or (fAsyncCmdPool == NULL) or (fRCBCmdPool == NULL) )
		status = kIOReturnNoMemory;
		
    return status;
}

IOFWIPMBufCommand *IOFWIPBusInterface::getMBufCommand()
{
	IOFWIPMBufCommand * mBufCommand = NULL;

	if( fMbufCmdPool )
		mBufCommand = (IOFWIPMBufCommand *)fMbufCmdPool->getCommand(false);

	if(mBufCommand == NULL) 
	{	
		if( fCurrentMBufCommands < kMaxAsyncCommands )
		{
			mBufCommand = new IOFWIPMBufCommand;
			if(not mBufCommand->init()) 
			{
				mBufCommand->release();
				mBufCommand = NULL;
			}
			else
				fCurrentMBufCommands++;
		}
	}

	return mBufCommand;
}

IOFWIPAsyncWriteCommand *IOFWIPBusInterface::getAsyncCommand(bool block, bool *deferNotify)
{
	IOFWIPAsyncWriteCommand * cmd = (IOFWIPAsyncWriteCommand *)fAsyncCmdPool->getCommand(block);

	if(cmd == NULL) 
	{	
		if( fCurrentAsyncIPCommands < kMaxAsyncCommands )
		{
			cmd = new IOFWIPAsyncWriteCommand;

			FWAddress addr;
			// setup block write
			addr.addressHi   = 0xdead;
			addr.addressLo   = 0xbabeface;
			
			if(not cmd->initAll(fIPLocalNode, fMaxTxAsyncDoubleBuffer, addr, txCompleteBlockWrite, this, false)) 
			{
				cmd->release();
				cmd = NULL;
			}
			else
			{
				fCurrentAsyncIPCommands++;
				fAsyncTransitSet->setObject(cmd);
			}
		}
	}

	if( cmd )
	{
		fIPLocalNode->fActiveCmds++;	
	}
	else
	{
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		fIPLocalNode->fNoCommands++;
	}

	if((fIPLocalNode->fActiveCmds - fIPLocalNode->fInActiveCmds) >= fLowWaterMark)
		*deferNotify = false;
	
	return cmd;
}

void IOFWIPBusInterface::returnAsyncCommand(IOFWIPAsyncWriteCommand *cmd)
{
	if(cmd->notDoubleComplete())
	{
		if(fAsyncCmdPool != NULL)
			fAsyncCmdPool->returnCommand(cmd);
		else
			cmd->release();
		fIPLocalNode->fInActiveCmds++;	
	}
	else
		fIPLocalNode->fDoubleCompletes++;
}

/*!
	@function initAsyncStreamCmdPool
	@abstract constructs AsyncStreamcommand objects and queues them in the pool
	@param none.
	@result Returns kIOReturnSuccess if it was successful, else kIOReturnNoMemory.
*/

UInt32 IOFWIPBusInterface::initAsyncStreamCmdPool()
{
    IOReturn status = kIOReturnSuccess;
    int		i = 0;

	if(fAsyncStreamTxCmdPool == NULL)
		fAsyncStreamTxCmdPool = IOCommandPool::withWorkLoop(workLoop);

	IOFWIPAsyncStreamTxCommand *cmd2 = NULL;
	
	for(i=0; i<=kMaxAsyncStreamCommands; i++){
        
        // Create a IP Async write command 
        cmd2 = new IOFWIPAsyncStreamTxCommand;
        if(!cmd2) {
            status = kIOReturnNoMemory;
            break;
        }

        // Initialize the write command
        if(!cmd2->initAll(fIPLocalNode, fControl, 0, 0, 0, GASP_TAG, fMaxTxAsyncDoubleBuffer, 
						kFWSpeed100MBit, txCompleteAsyncStream, this)) {
            status = kIOReturnNoMemory;
			cmd2->release();
            break;
        }

        // Queue the command in the command pool
        fAsyncStreamTxCmdPool->returnCommand(cmd2);
		fAsyncStreamTransitSet->setObject(cmd2);
    }
	
    return status;
}

										
/*!
	@function freeIPCmdPool
	@abstract frees both Async and AsyncStream command objects from the pool
	@param none.
	@result void.
*/
void IOFWIPBusInterface::freeAsyncCmdPool()
{
    IOFWIPAsyncWriteCommand	*cmd1 = NULL;
	UInt32 freeCount = 0;

    if(fAsyncCmdPool == NULL)
        return;

	// Should block till all outstanding commands are freed
    do
	{
        cmd1 = (IOFWIPAsyncWriteCommand*)fAsyncCmdPool->getCommand(false);
        if(cmd1 != NULL)
		{
			freeCount++;
            // release the command
            cmd1->release();
        }
    }while(cmd1 != NULL);
    
	fAsyncCmdPool->release();
	fAsyncCmdPool = NULL;
}
										
/*!
	@function freeIPCmdPool
	@abstract frees both Async and AsyncStream command objects from the pool
	@param none.
	@result void.
*/
void IOFWIPBusInterface::freeAsyncStreamCmdPool()
{
	if(fAsyncStreamTxCmdPool == NULL)
		return;

	IOFWIPAsyncStreamTxCommand *cmd2 = NULL;
	UInt32 freeCount = 0;

	// Should block till all outstanding commands are freed
	do{
        cmd2 = (IOFWIPAsyncStreamTxCommand*)fAsyncStreamTxCmdPool->getCommand(false);
        if(cmd2 != NULL)
		{
			freeCount++;
            // release the command
            cmd2->release();
        }
    }while(cmd2 != NULL);
    
	fAsyncStreamTxCmdPool->release();
	fAsyncStreamTxCmdPool = NULL;

    return;
}

IOReturn IOFWIPBusInterface::stopReceivingBroadcast()
{
	return fBroadcastReceiveClient->stop();
}

IOReturn IOFWIPBusInterface::startReceivingBroadcast(IOFWSpeed speed)
{
	return fBroadcastReceiveClient->start(speed);
}

IOUpdateARPCache IOFWIPBusInterface::getARPCacheHandler() const
{
	return (IOUpdateARPCache) &IOFWIPBusInterface::staticUpdateARPCache;
}

IOTransmitPacket IOFWIPBusInterface::getOutputHandler() const
{
    return (IOTransmitPacket) &IOFWIPBusInterface::staticOutputPacket;
}

/*!
	@function initDRBwithDevice
	@abstract create device reference block for a device object.
	@param lcb - the firewire link control block for this interface.
	@param eui64 - global unique id of a device on the bus.
	@param fDevObj - IOFireWireNub that has to be linked with the device reference block.
	@param itsMac - Indicates whether the destination is Macintosh or not.
	@result DRB* - pointer to the device reference block.
*/
DRB *IOFWIPBusInterface::initDRBwithDevice(UWIDE eui64, IOFireWireNub *device, bool itsMac)
{
	fIPLocalNode->closeIPGate();

	DRB   *drb = getDrbFromEui64(eui64);   

	if(not drb)
	{
		if ((drb = new DRB) == NULL)
		{
			fIPLocalNode->openIPGate();
			return NULL;
		}
	}
	
	CSRNodeUniqueID fwuid = device->getUniqueID();
	if(itsMac)
		fIPLocalNode->makeEthernetAddress(&fwuid, drb->fwaddr, GUID_TYPE);
	else
		fIPLocalNode->getBytesFromGUID((void*)(&fwuid), drb->fwaddr, GUID_TYPE);
		
	drb->deviceID	= (UInt32)device;
	drb->eui64		= eui64;
	drb->itsMac		= itsMac;
	drb->maxSpeed	= kFWSpeed100MBit;      
	drb->maxSpeed	= device->FWSpeed();
	drb->maxPayload	= device->maxPackLog(true);
	
	activeDrb->setObject(drb);

	fIPLocalNode->openIPGate();

    return drb;
}

/*!
	@function getMTU
	@abstract returns the MTU (Max Transmission Unit) supported by the IOFireWireIP.
	@param None.
	@result UInt32 - MTU value.
*/
UInt32 IOFWIPBusInterface::getMTU()
{
    return FIREWIRE_MTU;
}

UInt32 IOFWIPBusInterface::outputPacket(mbuf_t pkt, void * param)
{
	register struct firewire_header *fwh;
	int	status = kIOReturnError;
	
	fwh = (struct firewire_header*)mbuf_data(pkt);
	
	switch(htons(fwh->fw_type))
	{
		case FWTYPE_IPV6:
			addNDPOptions(pkt);
			status = txIP(pkt, fLcb->ownNodeID, fLcb->busGeneration, fLcb->ownMaxPayload, fLcb->maxBroadcastPayload, fLcb->maxBroadcastSpeed, FWTYPE_IPV6);
			break;
			
		case FWTYPE_IP:
			status = txIP(pkt, fLcb->ownNodeID, fLcb->busGeneration, fLcb->ownMaxPayload, fLcb->maxBroadcastPayload, fLcb->maxBroadcastSpeed, FWTYPE_IP);
			break;

		case FWTYPE_ARP:
			status = txARP(pkt, fLcb->ownNodeID, fLcb->busGeneration, fLcb->maxBroadcastSpeed);
			break;
			
		default :
			fIPLocalNode->freePacket((struct mbuf*)pkt);
			break;
	}

	if(status == kIOFireWireOutOfTLabels)
	{
		status = kIOReturnOutputStall;
		
		if((fIPLocalNode->fActiveCmds - fIPLocalNode->fInActiveCmds)  <= 1)
			fIPLocalNode->fServiceInOutput++;
		
		if((fIPLocalNode->transmitQueue->getSize() > fIPLocalNode->fMaxQueueSize)
			|| ((fIPLocalNode->fActiveCmds - fIPLocalNode->fInActiveCmds)  <= 1))
		{
			// So far, too many stalls. Sink the packets, till we have manageable queue
			fIPLocalNode->freePacket((struct mbuf*)pkt);
			fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
			status = kIOReturnOutputDropped;
		}
	}
	else
		status = kIOReturnOutputSuccess;

    return status;
}

/*!
	@function txComplete
	@abstract Callback for the Async write complete 
	@param refcon - callback data.
    @param status - status of the command.
    @param device - device that the command was send to.
    @param fwCmd - command object which generated the transaction.
	@result void.
*/
void IOFWIPBusInterface::txCompleteBlockWrite(void *refcon, IOReturn status, IOFireWireNub *device, IOFWCommand *fwCmd)
{
    IOFWIPBusInterface			*fwIPPriv	= (IOFWIPBusInterface*)refcon;
	
	if(not fwIPPriv)
		return;
		
	IOFireWireIP				*fwIPObject	= OSDynamicCast(IOFireWireIP, fwIPPriv->fIPLocalNode);
	
	if(not fwIPObject)
		return;
		
    IOFWIPAsyncWriteCommand		*cmd		= OSDynamicCast(IOFWIPAsyncWriteCommand, fwCmd);
	
	if(not cmd)
		return;
	
	// fwIPObject->closeIPGate();
	
	// Only in case of kIOFireWireOutOfTLabels, we ignore freeing of Mbuf
	if(status == kIOReturnSuccess)
	{
		// We get callback 1 packet at a time, so we can increment by 1
		fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->outputPackets);
		fwIPObject->fTxUni++;
	}
	else 
	{
		// Increment error output packets
		fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->outputErrors);
		fwIPObject->fCallErrs++;
	}
	
	cmd->resetDescriptor(status);
	
	fwIPPriv->returnAsyncCommand(cmd);
	// Fix to over kill servicing the queue 
	if ((fwIPObject->fActiveCmds - fwIPObject->fInActiveCmds)  <= fwIPPriv->fLowWaterMark)
	{
		fwIPObject->transmitQueue->service( IOBasicOutputQueue::kServiceAsync );
		fwIPObject->fServiceInCallback++;
	}
	
	// fwIPObject->openIPGate();

    return;
}

/*!
	@function txAsyncStreamComplete
	@abstract Callback for the Async stream transmit complete 
	@param refcon - callback 
	@param status - status of the command.
	@param bus information.
	@param fwCmd - command object which generated the transaction.
	@result void.
*/
void IOFWIPBusInterface::txCompleteAsyncStream(void *refcon, IOReturn status, 
										IOFireWireBus *bus, IOFWAsyncStreamCommand *fwCmd)
{
    IOFWIPBusInterface			*fwIPPriv	= (IOFWIPBusInterface*)refcon;

	if(not fwIPPriv)
		return;

	IOFireWireIP				*fwIPObject	= OSDynamicCast(IOFireWireIP, fwIPPriv->fIPLocalNode);
	
	if(not fwIPObject)
		return;

    IOFWIPAsyncStreamTxCommand	*cmd		= OSDynamicCast(IOFWIPAsyncStreamTxCommand, fwCmd);
		
	if(not cmd)
		return;
			
	// fwIPObject->closeIPGate();

	if(status == kIOReturnSuccess)
	{
		fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->outputPackets);
		fwIPObject->fTxBcast++;
	}
	else
		fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->outputErrors);

	if(fwIPPriv->fAsyncStreamTxCmdPool != NULL) 		// Queue the command back into the command pool
	{
		fwIPPriv->fAsyncStreamTxCmdPool->returnCommand(cmd);
		fwIPObject->fInActiveBcastCmds++;
	}
	
	// fwIPObject->openIPGate();

    return;
}

/*!
	@function txARP
	@abstract Transmit ARP request or response.
	@param m - mbuf containing the ARP packet.
	@result void.
*/
SInt32 IOFWIPBusInterface::txARP(mbuf_t m, UInt16 nodeID, UInt32 busGeneration, IOFWSpeed speed)
{
	IOReturn status = kIOReturnSuccess;
	
	if(fAsyncStreamTxCmdPool == NULL)
		status = initAsyncStreamCmdPool();
	
	// Get an async command from the command pool
	IOFWIPAsyncStreamTxCommand	*cmd = (IOFWIPAsyncStreamTxCommand*)fAsyncStreamTxCmdPool->getCommand(false);
		
	// Lets not block to get a command, IP may retry soon ..:)
	if(cmd == NULL)
	{
		// Error, so we touch the error output packets
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		fIPLocalNode->fNoBCastCommands++;
		return status;
	}

	fIPLocalNode->fActiveBcastCmds++;

    IORecursiveLockLock(fIPLock);

	mbuf_t n = m;
		
	// Get the buffer pointer from the command pool
	UInt8	*buf = (UInt8*)cmd->getBufferFromDesc();
	UInt32	dstBufLen = cmd->getMaxBufLen();
	
	UInt32	offset = sizeof(struct firewire_header);
	UInt32	cmdLen = mbuf_pkthdr_len(m) - offset;
	
	// Construct the GASP_HDR and Unfragment header
	struct arp_packet *fwa_pkt = (struct arp_packet*)(buf);
    bzero((caddr_t)fwa_pkt, sizeof(*fwa_pkt));
    
	// Fill the GASP fields 
    fwa_pkt->gaspHdr.sourceID = htons(nodeID);
    memcpy(&fwa_pkt->gaspHdr.gaspID, &gaspVal, sizeof(GASP_ID));
	
	// Set the unfragmented header information
    fwa_pkt->ip1394Hdr.etherType = htons(FWTYPE_ARP);
	// Modify the buffer pointer
	buf += (sizeof(GASP_HDR) + sizeof(IP1394_UNFRAG_HDR));
	// Copy the arp packet into the buffer
	mbufTobuffer(n, &offset, buf, dstBufLen, cmdLen);
	// Update the length to have the GASP and IP1394 Header
	cmdLen += (sizeof(GASP_HDR) + sizeof(IP1394_UNFRAG_HDR));
	
	// Initialize the command with new values of device object
	status = cmd->reinit( busGeneration, 
						  DEFAULT_BROADCAST_CHANNEL, 
						  cmdLen,
						  speed,
						  txCompleteAsyncStream, 
						  this);
					
	if(status == kIOReturnSuccess)
		status = cmd->submit();
	else
	{
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		fAsyncStreamTxCmdPool->returnCommand(cmd);
		fIPLocalNode->fInActiveBcastCmds++;
	}

	if(status != kIOReturnSuccess)
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
	
	fIPLocalNode->freePacket((struct mbuf*)m);
					
    IORecursiveLockUnlock(fIPLock);
					
	return status;
}

SInt32	IOFWIPBusInterface::txBroadcastIP(const mbuf_t m, UInt16 nodeID, UInt32 busGeneration, 
											UInt16 ownMaxPayload, UInt16 maxBroadcastPayload, 
											IOFWSpeed speed, const UInt16 type)
{
	UInt16 datagramSize = mbuf_pkthdr_len(m) - sizeof(struct firewire_header);

	fIPLocalNode->fMaxPktSize = max(datagramSize,fIPLocalNode->fMaxPktSize);
	
	UInt16 maxPayload = MIN((UInt16)1 << maxBroadcastPayload, (UInt16)1 << ownMaxPayload);

	IOReturn	status = ENOBUFS;
	// Asynchronous stream datagrams are never fragmented!
	if (datagramSize + sizeof(IP1394_UNFRAG_HDR) > maxPayload)
	{
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		fIPLocalNode->freePacket((struct mbuf*)m);
		return status;
	}

	// create a command pool on demand
	if(fAsyncStreamTxCmdPool == NULL)
		initAsyncStreamCmdPool();
	
	// Get an async command from the command pool
	IOFWIPAsyncStreamTxCommand *asyncStreamCmd = (IOFWIPAsyncStreamTxCommand*)fAsyncStreamTxCmdPool->getCommand(false);
	
	// Lets not block to get a command, IP may retry soon ..:)
	if(asyncStreamCmd == NULL)
	{
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		fIPLocalNode->freePacket((struct mbuf*)m);
		fIPLocalNode->fNoBCastCommands++;
		return status;
	}

	fIPLocalNode->fActiveBcastCmds++;

    IORecursiveLockLock(fIPLock);
	
	// Get the buffer pointer from the command pool
	UInt8 *buf = (UInt8*)asyncStreamCmd->getBufferFromDesc();
	UInt32 dstBufLen = asyncStreamCmd->getMaxBufLen();
	
	// Get it assigned to the header
	GASP_HDR *gaspHdr = (GASP_HDR *)buf;
	gaspHdr->sourceID = htons(nodeID);	
	memcpy(&gaspHdr->gaspID, &gaspVal, sizeof(GASP_ID));
	
	IP1394_ENCAP_HDR *ip1394Hdr = (IP1394_ENCAP_HDR*)((UInt8*)buf + sizeof(GASP_HDR));
	ip1394Hdr->singleFragment.etherType = htons(type);
	ip1394Hdr->singleFragment.reserved = htons(UNFRAGMENTED);
	
	UInt32 cmdLen = datagramSize;
	UInt32 offset = sizeof(struct firewire_header);
	UInt16 headerSize = sizeof(GASP_HDR) + sizeof(IP1394_UNFRAG_HDR);
	
	// Increment the buffer pointer for the unfrag or frag header
	buf += headerSize;
	
	mbufTobuffer(m, &offset, buf, dstBufLen, cmdLen);

	cmdLen += headerSize;

	// Initialize the command with new values of device object
	status = asyncStreamCmd->reinit(busGeneration, DEFAULT_BROADCAST_CHANNEL, 
									cmdLen, speed, txCompleteAsyncStream, this);

	if(status == kIOReturnSuccess)
		status = asyncStreamCmd->submit();
	else
	{
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		fAsyncStreamTxCmdPool->returnCommand(asyncStreamCmd);
		fIPLocalNode->fInActiveBcastCmds++;
	}

	if(status != kIOReturnSuccess)
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
	
	fIPLocalNode->freePacket((struct mbuf*)m);

    IORecursiveLockUnlock(fIPLock);

	return status;
}

SInt32 IOFWIPBusInterface::txUnicastUnFragmented(IOFireWireNub *device, const FWAddress addr, const mbuf_t m, const UInt16 pktSize, const UInt16 type)
{
	SInt32 status = kIOReturnSuccess;

	bool deferNotify = true;

	IOFWIPMBufCommand * mBufCommand = getMBufCommand();

	if( not mBufCommand )
	{
		fIPLocalNode->freePacket((struct mbuf*)m);
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		return status;
	}

	mBufCommand->reinit(m, fIPLocalNode, fMbufCmdPool);

	IOFWIPAsyncWriteCommand *cmd = getAsyncCommand(false, &deferNotify); // Get an async command from the command pool

	mBufCommand->retain();
	
	// Lets not block to get a command, IP may retry soon ..:)
	if(cmd) 
	{
		// All done in one gulp!
		IP1394_ENCAP_HDR *ip1394Hdr = (IP1394_ENCAP_HDR*)cmd->initPacketHeader(mBufCommand, kCopyBuffers, UNFRAGMENTED, 
																				 sizeof(IP1394_UNFRAG_HDR), 
																				 sizeof(struct firewire_header));
		ip1394Hdr->fragment.datagramSize = htons(UNFRAGMENTED);
		ip1394Hdr->singleFragment.etherType = htons(type);
		ip1394Hdr->singleFragment.reserved = 0;
		
		status = cmd->transmit (device, pktSize, addr, txCompleteBlockWrite, this, true,
								deferNotify, kQueueCommands);

	}

	mBufCommand->releaseWithStatus(status);

	if( status != kIOFireWireOutOfTLabels )
		fIPLocalNode->activeMbufs++;

	return status;
}

SInt32 IOFWIPBusInterface::txUnicastFragmented(IOFireWireNub *device, const FWAddress addr, const mbuf_t m, 
											const UInt16 pktSize, const UInt16 type, UInt16 maxPayload, UInt16 dgl)
{
	UInt32	residual = pktSize;
	UInt32	fragmentOffset = 0;
	UInt32	offset = sizeof(struct firewire_header);
	SInt32	status = kIOReturnSuccess;
	bool	deferNotify = true;

	IOFWIPMBufCommand * mBufCommand = getMBufCommand();

	if( not mBufCommand )
	{
		fIPLocalNode->freePacket((struct mbuf*)m);
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		return status;
	}
	
	mBufCommand->reinit(m, fIPLocalNode, fMbufCmdPool);
	mBufCommand->retain();
	
	while (residual) 
	{
		deferNotify = true;
		status		= kIOReturnSuccess;
		
		IOFWIPAsyncWriteCommand *cmd = getAsyncCommand(false, &deferNotify); // Get an async command from the command pool
	
		// Lets not block to get a command, IP may retry soon ..:)
		if(not cmd) 
			break;
		
		// false - don't copy , if true - copy the packets
		fIPLocalNode->fTxFragmentPkts++;
		FragmentType fragmentType = FIRST_FRAGMENT;

		IP1394_ENCAP_HDR *ip1394Hdr = (IP1394_ENCAP_HDR*)cmd->initPacketHeader(mBufCommand, kCopyBuffers, fragmentType, 
																	sizeof(IP1394_FRAG_HDR), 
																	offset);

		// Distinguish first, interior and last fragments
		UInt32 cmdLen = MIN(residual, maxPayload - sizeof(IP1394_FRAG_HDR));
				
		ip1394Hdr->fragment.datagramSize = htons(pktSize - 1);
		
		if (fragmentOffset == 0) 
			ip1394Hdr->singleFragment.etherType = htons(type);
		else 
		{
			ip1394Hdr->fragment.fragmentOffset = htons(fragmentOffset);
			fragmentType = (cmdLen < residual) ? INTERIOR_FRAGMENT : LAST_FRAGMENT;
		}
		
		// Get your datagram labels correct 
		ip1394Hdr->fragment.datagramSize	|=	htons(fragmentType << 14);
		ip1394Hdr->fragment.dgl				=	htons(dgl);
		ip1394Hdr->fragment.reserved		=	0;

		status = cmd->transmit (device, cmdLen, addr, txCompleteBlockWrite, this, true,
								deferNotify, kQueueCommands, fragmentType);
		
		if(status != kIOReturnSuccess)
			break;
		
		fragmentOffset	+= cmdLen;	// Account for the position and...
		offset			+= cmdLen;
		residual		-= cmdLen;  // ...size of the fragment just sent
	}

	mBufCommand->releaseWithStatus(status);

	if( status != kIOFireWireOutOfTLabels )
		fIPLocalNode->activeMbufs++;

	return status;
}

SInt32 IOFWIPBusInterface::txUnicastIP(mbuf_t m, UInt16 nodeID, UInt32 busGeneration, UInt16 ownMaxPayload, IOFWSpeed speed, const UInt16 type)
{
	struct firewire_header *fwh = (struct firewire_header *)mbuf_data(m);

    IORecursiveLockLock(fIPLock);

	ARB *arb = getArbFromFwAddr(fwh->fw_dhost);

	SInt32 status = EHOSTUNREACH;
	
	if(arb == NULL)
	{
		fIPLocalNode->freePacket((struct mbuf*)m);
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		IORecursiveLockUnlock(fIPLock);
		return status;
	}
	
	TNF_HANDLE *handle = &arb->handle; 

	// Node had disappeared, but entry exists for specified timer value
	if(handle->unicast.deviceID == kInvalidIPDeviceRefID) 
	{
		fIPLocalNode->freePacket((struct mbuf*)m);
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		IORecursiveLockUnlock(fIPLock);
		return status;
	}
	
	// Get the actual length of the packet from the mbuf
	UInt16 datagramSize = mbuf_pkthdr_len(m) - sizeof(struct firewire_header);
	UInt16 residual = datagramSize;
	
	// setup block write
	FWAddress addr; 
	addr.addressHi   = handle->unicast.unicastFifoHi;
	addr.addressLo   = handle->unicast.unicastFifoLo;
	IOFireWireNub	*device = (IOFireWireNub*)handle->unicast.deviceID;
	
	// Calculate the payload and further down will decide the fragmentation based on that
	UInt32 drbMaxPayload = 1 << device->maxPackLog(true, addr);

	UInt32 maxPayload = MIN((UInt32)1 << (handle->unicast.maxRec+1), (UInt32)1 << fLcb->ownMaxPayload);
	maxPayload = MIN(drbMaxPayload, maxPayload);

	fIPLocalNode->fMaxPacketSize = maxPayload;

	UInt16 dgl = 0;
	bool unfragmented = false;
	// Only fragments use datagram label
	if (!(unfragmented = ((datagramSize + sizeof(IP1394_UNFRAG_HDR)) <= maxPayload)))
		dgl = fLcb->datagramLabel++; 
  
	if (unfragmented)
		status = txUnicastUnFragmented(device, addr, m, residual, type);
	else
		status = txUnicastFragmented(device, addr, m, residual, type, maxPayload, dgl);
	
    IORecursiveLockUnlock(fIPLock);
		
	return status;
}

/*!
	@function txIP
	@abstract Transmit IP packet.
	@param m - mbuf containing the IP packet.
	@param type - type of the packet (IPv6 or IPv4).
    @result void.
*/
SInt32 IOFWIPBusInterface::txIP(mbuf_t m, UInt16 nodeID, UInt32 busGeneration, UInt16 ownMaxPayload, UInt16 maxBroadcastPayload, IOFWSpeed speed, UInt16 type)
{
	// If its not a packet header
	if(not (mbuf_flags(m) & M_PKTHDR))
	{
		fIPLocalNode->freePacket((struct mbuf*)m);
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		return kIOReturnError;
	}
	
	struct firewire_header *fwh = (struct firewire_header *)mbuf_data(m);
	
	SInt32 status = kIOReturnSuccess;
	
	if(bcmp(fwh->fw_dhost, fwbroadcastaddr, FIREWIRE_ADDR_LEN) == 0 ||
	   bcmp(fwh->fw_dhost, ipv4multicast, FIREWIREMCAST_V4_LEN) == 0 || 
	   bcmp(fwh->fw_dhost, ipv6multicast, FIREWIREMCAST_V6_LEN) == 0)
		status = txBroadcastIP(m, nodeID, busGeneration, ownMaxPayload, maxBroadcastPayload, speed, type);
	else
		status = txUnicastIP(m, nodeID, busGeneration, ownMaxPayload, speed, type);
	
	return status;
}

/*!
	@function txMCAP
	@abstract This procedure transmits either an MCAP solicitation or advertisement on the
			  default broadcast channel, dependent upon whether or not an MCB is supplied.
			  Note that if more than one multicast address group is associated with a
			  particular channel that multiple MCAP group descriptors are created.
 	@param lcb - link control block of the local node.
	@param mcb - multicast control block.
	@param ipAddress - IP address.
	@result void.
*/
void IOFWIPBusInterface::txMCAP(LCB *lcb, MCB *mcb, UInt32 ipAddress){
	MARB *arb;
	MCAST_DESCR *groupDescriptor;
	IOFWIPAsyncStreamTxCommand *asyncStreamCmd = NULL;
	struct mcap_packet *packet;
	IOReturn status;
	UInt32 cmdLen = 0;
	UInt8 *buf;

	if(fAsyncStreamTxCmdPool == NULL)
		initAsyncStreamCmdPool();
	
	// Get an async command from the command pool
	asyncStreamCmd = (IOFWIPAsyncStreamTxCommand*)fAsyncStreamTxCmdPool->getCommand(false);
		
	// Lets not block to get a command, IP may retry soon ..:)
	if(asyncStreamCmd == NULL)
	{
		fIPLocalNode->fNoBCastCommands++;
		return;
	}

	fIPLocalNode->fActiveBcastCmds++;
			
	// Get the buffer pointer from the command pool
	buf = (UInt8*)asyncStreamCmd->getBufferFromDesc();
	// dstBufLen = asyncStreamCmd->getMaxBufLen();

	packet = (struct mcap_packet*)buf;
	memset(packet, 0, sizeof(*packet));
	packet->gaspHdr.sourceID = htons(fLcb->ownNodeID);
	memcpy(&packet->gaspHdr.gaspID, &gaspVal, sizeof(GASP_ID));
	packet->ip1394Hdr.etherType = htons(ETHER_TYPE_MCAP);
	packet->mcap.length = sizeof(*packet);          /* Fix endian-ness later */
	groupDescriptor = packet->mcap.groupDescr;
	
	if (mcb != NULL) 
	{
		packet->mcap.opcode = MCAP_ADVERTISE;
		
		OSCollectionIterator * iterator = OSCollectionIterator::withCollection( multicastArb );
		while( NULL != (arb = OSDynamicCast(MARB, iterator->getNextObject())) )
		{
			if (arb->handle.multicast.channel == mcb->channel) 
			{
				memcpy(&ipAddress, &arb->ipAddress, sizeof(ipAddress));
				memset(groupDescriptor, 0, sizeof(MCAST_DESCR));
				groupDescriptor->length = sizeof(MCAST_DESCR);
				groupDescriptor->type = MCAST_TYPE;
				groupDescriptor->expiration = mcb->expiration;
				groupDescriptor->channel = mcb->channel;
				groupDescriptor->speed = arb->handle.multicast.spd;
				groupDescriptor->groupAddress = arb->ipAddress;
				groupDescriptor = (MCAST_DESCR*)((UInt32) groupDescriptor + sizeof(MCAST_DESCR));
				packet->mcap.length += sizeof(MCAST_DESCR);
			}
		}
		iterator->release();
	}
	else 
	{
		packet->mcap.opcode = MCAP_SOLICIT;
		memset(groupDescriptor, 0, sizeof(MCAST_DESCR));
		groupDescriptor->length = sizeof(MCAST_DESCR);
		groupDescriptor->type = MCAST_TYPE;
		groupDescriptor->groupAddress = ipAddress;
		packet->mcap.length += sizeof(MCAST_DESCR);
	}

   cmdLen = packet->mcap.length;   // In CPU byte order 
   packet->mcap.length = htons(packet->mcap.length); // Serial Bus order

	// Initialize the command with new values of device object
	status = asyncStreamCmd->reinit(fLcb->busGeneration, 
						DEFAULT_BROADCAST_CHANNEL, 
						cmdLen,
						fLcb->maxBroadcastSpeed,
						txCompleteAsyncStream, 
						this);
					  
	if(status == kIOReturnSuccess)
		status = asyncStreamCmd->submit();
	else
	{
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
		fAsyncStreamTxCmdPool->returnCommand(asyncStreamCmd);
		fIPLocalNode->fInActiveBcastCmds++;
	}

	if(status != kIOReturnSuccess)
		fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->outputErrors);
}

/*!
	@function rxUnicastFlush
	@abstract Starts the batch processing of the packets, its
	          already on its own workloop.
*/
void IOFWIPBusInterface::rxUnicastFlush()
{
	UInt32 count = 0;

    IORecursiveLockLock(fIPLock);
	
	if(fIPLocalNode->fPacketsQueued = true)
	{
		count = fIPLocalNode->networkInterface->flushInputQueue();
        if(count > fIPLocalNode->fMaxInputCount)
            fIPLocalNode->fMaxInputCount = count; 

		fIPLocalNode->fPacketsQueued = false;
	}

    IORecursiveLockUnlock(fIPLock);

	return;
}

/*!
	@function rxUnicastComplete
	@abstract triggers the indication workloop to do batch processing
				of incoming packets.
*/
void IOFWIPBusInterface::rxUnicastComplete(void *refcon)
{
	IOFWIPBusInterface *fwIPPriv = (IOFWIPBusInterface*)refcon;

	fwIPPriv->rxUnicastFlush();

	return;
}

/*!
	@function rxUnicast
	@abstract block write handler. Handles both ARP and IP packet.
*/
UInt32 IOFWIPBusInterface::rxUnicast( void		*refcon,
								UInt16		nodeID,
                                IOFWSpeed	&speed,
                                FWAddress	addr,
                                UInt32		len,
								const void	*buf,
								IOFWRequestRefCon requestRefcon)
{
	IOFWIPBusInterface		*fwIPPriv = (IOFWIPBusInterface*)refcon;
	IOFireWireIP			*fwIPObject	= OSDynamicCast(IOFireWireIP, fwIPPriv->fIPLocalNode);
	IP1394_UNFRAG_HDR		*ip1394Hdr	= (IP1394_UNFRAG_HDR *)buf;

	if(not fwIPPriv->fStarted)
		return kIOReturnSuccess;
   
	UInt8	lf = (htons(ip1394Hdr->reserved) >> 14);
	// Handle the unfragmented packet
	if (lf == UNFRAGMENTED) 
	{
		void	*datagram		= (void *) ((ULONG) ip1394Hdr + sizeof(IP1394_UNFRAG_HDR));
		UInt16	datagramSize	= len - sizeof(IP1394_UNFRAG_HDR);
		UInt16	type			= ntohs(ip1394Hdr->etherType);

		switch (type) 
		{
			case FWTYPE_IPV6:
			case FWTYPE_IP:
				if (datagramSize >= IPV4_HDR_SIZE && datagramSize <= FIREWIRE_MTU)
					fwIPPriv->rxIP(datagram, datagramSize, FW_M_UCAST, type);
				break;

			case FWTYPE_ARP:
				if (datagramSize >= sizeof(IP1394_ARP) && datagramSize <= FIREWIRE_MTU)
					fwIPPriv->rxARP((IP1394_ARP*)datagram, FW_M_UCAST);
				break;
			
			default :
				// Unknown packet type
				fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->inputErrors);
				break;
		}
	}
	else
	{     
		fwIPObject->fRxFragmentPkts++;
  
		if(fwIPPriv->rxFragmentedUnicast(nodeID, (IP1394_FRAG_HDR*)ip1394Hdr, len) == kIOReturnError)
			fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->inputErrors);
	}

	fwIPObject->fRxUni++;
	
	return kIOReturnSuccess;
}
	
IOReturn IOFWIPBusInterface::rxFragmentedUnicast(UInt16 nodeID, IP1394_FRAG_HDR *pkt, UInt32 len)
{
	IP1394_FRAG_HDR *fragmentHdr	= (IP1394_FRAG_HDR*) pkt;  // Different header layout
	void			*fragment		= (void *) ((UInt32) fragmentHdr + sizeof(IP1394_FRAG_HDR));
	UInt16			fragmentSize	= len - sizeof(IP1394_FRAG_HDR);
	
	UInt8	lf				= htons(fragmentHdr->datagramSize) >> 14;
	UInt16	datagramSize	= (htons(fragmentHdr->datagramSize) & 0x3FFF) + 1;
	UInt16	label			= htons(fragmentHdr->dgl);
	
	if(datagramSize > FIREWIRE_MTU)
		return kIOReturnError;

	recursiveScopeLock lock(fIPLock);
	
	IOReturn result			= kIOReturnSuccess;
	UInt16	 fragmentOffset = htons(fragmentHdr->fragmentOffset);

	RCB *rcb = getRcb(nodeID, label);

	if (rcb == NULL) 
	{
		if (lf == FIRST_FRAGMENT) 
		{
			mbuf_t rxMBuf = (mbuf_t)allocateMbuf(datagramSize + sizeof(firewire_header));

			if (rxMBuf == NULL)
			{
				fIPLocalNode->fNoMbufs++;
				return kIOReturnError;
			}
			
			if ((rcb = getRCBCommand( nodeID, label, fragmentOffset, datagramSize, rxMBuf )) == NULL) 
			{
				fIPLocalNode->fNoRCBCommands++;
				cleanRCBCache();
				fIPLocalNode->freePacket((struct mbuf*)rxMBuf, 0);
				return kIOReturnError;
			}
		 
			// Make space for the firewire header to be helpfull in firewire_demux
			struct firewire_header *fwh = (struct firewire_header *)mbuf_data(rxMBuf);
			bzero(fwh, sizeof(struct firewire_header));
			// when indicating to the top layer
			// JLIU - fragmentHdr already in network order, do not swap fragmentOffset
			fwh->fw_type  = fragmentHdr->fragmentOffset;
			rcb->residual = rcb->datagramSize;

			activeRcb->setObject(rcb);
			fragmentOffset = 0;
		}
		else 
			result = kIOReturnError;
	}

	if( result == kIOReturnSuccess )
	{
		UInt16 amountToCopy = MIN(fragmentSize, rcb->datagramSize - fragmentOffset);
		
		if(amountToCopy > rcb->residual)
		{
			fIPLocalNode->fRxFragmentPktsDropped++;
			result = kIOReturnError;
		}
		else
		{
			bufferToMbuf(rcb->mBuf, sizeof(struct firewire_header)+fragmentOffset, (UInt8*)fragment, amountToCopy);
			
			rcb->residual -= MIN(fragmentSize, rcb->residual); 

			if ( rcb->residual == 0 ) 
			{           
				// Legitimate etherType ? this prevents corrupted etherType 
				// being presented to the networking layer
				if (rcb->etherType == FWTYPE_IP || rcb->etherType == FWTYPE_IPV6) 
					fIPLocalNode->receivePackets (rcb->mBuf, mbuf_pkthdr_len(rcb->mBuf), false);
				else
				{
					fIPLocalNode->freePacket((struct mbuf*)rcb->mBuf, 0); 
					result = kIOReturnError;
				}

				releaseRCB(rcb, false);
			}
		}
	}
	
	return result;
}

/*!
	@function rxAsyncStream
	@abstract callback for an Asyncstream packet, can be both IP or ARP packet.
			This procedure receives an indication when an asynchronous stream
			packet arrives on the default broadcast channel. The packet "should" be GASP,
			but we perform a few checks to make sure. Once we know these are OK, we check
			the etherType field in the unfragmented encapsulation header. This is necessary
			to dispatch the three types of packet that RFC 2734 permits on the default
			broadcast channel: an IPv4 datagram, and ARP request or response or a multi-
			channel allocation protocol (MCAP) message. The only remaining check, for each
			of these three cases, is to make sure that the packet is large enough to hold
			meaningful data. If so, send the packet to another procedure for further
			processing.  
	@param DCLCommandStruct *callProc.
	@result void.
*/
void IOFWIPBusInterface::rxAsyncStream(DCLCommandStruct *callProc){
    
	DCLCallProc 	*ptr = (DCLCallProc*)callProc;
	RXProcData		*proc = (RXProcData*)ptr->procData;
	
	IOFWIPBusInterface			*fwIPPriv = (IOFWIPBusInterface*)proc->obj;
	IOFireWireIP				*fwIPObject	= OSDynamicCast(IOFireWireIP, fwIPPriv->fIPLocalNode);
    IOFWAsyncStreamRxCommand	*fwRxAsyncStream;
	
	UInt8			*buffer = proc->buffer;
	void			*datagram;
	UInt16			datagramSize;
	GASP			*gasp = (GASP*)buffer;
	LCB 			*lcb = fwIPPriv->fLcb;
	ISOC_DATA_PKT	*pkt = (ISOC_DATA_PKT*)buffer; 
	UInt16 			type = 0;

	if(not fwIPPriv->fStarted)
		return;

    fwRxAsyncStream = (IOFWAsyncStreamRxCommand*)proc->thisObj;

    if( fwRxAsyncStream->modifyDCLJumps(callProc) == kIOReturnError)
		return;
    
	if(pkt->tag != GASP_TAG){
		// Error, so we touch the error output packets
		fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->inputErrors);
		fwIPObject->fGaspTagError++;
		return;
    }
	
	// Minimum size requirement
	if (gasp->dataLength < sizeof(GASP_HDR) + sizeof(IP1394_UNFRAG_HDR)) {
		fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->inputErrors);
		fwIPObject->fGaspHeaderError++;
		return;
    }

	// Ignore GASP if not specified by RFC 2734
	if (memcmp(&gasp->gaspHdr.gaspID, &gaspVal, sizeof(GASP_ID)) != 0) {
		fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->inputErrors);
		fwIPObject->fNonRFC2734Gasp++;
		return;
    }

	// Also ignore GASP if not from the local bus
	if ((htons(gasp->gaspHdr.sourceID) >> 6) != LOCAL_BUS_ID) {
		fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->inputErrors);
		fwIPObject->fRemoteGaspError++;
		return;
    }
   
	// Broadcast fragmentation not supported
	if (gasp->ip1394Hdr.reserved != htons(UNFRAGMENTED)) {
		fwIPObject->networkStatAdd(&(fwIPObject->getNetStats())->inputErrors);
		fwIPObject->fEncapsulationHeaderError++;
		return;
    }
   
   datagram = (void *) ((UInt32) buffer + sizeof(GASP));
   datagramSize = gasp->dataLength - (sizeof(GASP_HDR) + sizeof(IP1394_UNFRAG_HDR));
   type = ntohs(gasp->ip1394Hdr.etherType);
//   IOLog("   Ether type 0x%04X (data length %d)\n\r",htons(gasp->ip1394Hdr.etherType), datagramSize);
   
	switch (type) {
		case FWTYPE_IPV6:
		case FWTYPE_IP:
			if (datagramSize >= IPV4_HDR_SIZE && datagramSize <= FIREWIRE_MTU)
				fwIPPriv->rxIP(datagram, datagramSize, FW_M_BCAST, type);
			break;

		case FWTYPE_ARP:
			if (datagramSize >= sizeof(IP1394_ARP) && datagramSize <= FIREWIRE_MTU)
				fwIPPriv->rxARP((IP1394_ARP*)datagram, FW_M_BCAST);
			break;
			
		case ETHER_TYPE_MCAP:
			if (datagramSize >= sizeof(IP1394_MCAP) && datagramSize <= FIREWIRE_MTU)
				fwIPPriv->rxMCAP(lcb, htons(gasp->gaspHdr.sourceID), 
									(IP1394_MCAP*)datagram, datagramSize - sizeof(IP1394_MCAP));
			break;
	}

	fwIPObject->fRxBcast++;

	return;
}

/*!
	@function rxMCAP
	@abstract called from rxAsyncstream for processing MCAP advertisement.
			When an MCAP advertisement is received, parse all of its descriptors 
			looking for any that match group addreses in our MCAP cache. For those that 
			match, update  the channel number (it may have changed from the default
			broadcast channel or since the last advertisement), update the speed 
			(the MCAP owner may have changed the speed requirements as nodes joined or 
			left the group) and refresh the expiration timer so that the MCAP 
			channel is valid for another number of seconds into the future. 
			Th-th-th-that's all, folks!
	@param lcb - the firewire link control block for this interface.
    @param mcapSourceID - source nodeid which generated the multicast advertisement packet.
    @param mcap - mulitcast advertisment packet without the GASP header.
	@param dataSize - size of the packet.
	@result void.
*/
void IOFWIPBusInterface::rxMCAP(LCB *lcb, UInt16 mcapSourceID, IP1394_MCAP *mcap, UInt32 dataSize){

	MARB *arb;
	UInt32 currentChannel;
	MCAST_DESCR *groupDescr = mcap->groupDescr;
	MCB		*mcb,	*priorMcb;
	IOFWAsyncStreamRxCommand *asyncStreamRxClient;
	IOReturn ioStat = kIOReturnSuccess;

	if ((mcap->opcode != MCAP_ADVERTISE) && (mcap->opcode != MCAP_SOLICIT))
		return;        // Ignore reserved MCAP opcodes
	  
	dataSize = MIN(dataSize, htons(mcap->length) - sizeof(IP1394_MCAP));
   
	while (dataSize >= sizeof(MCAST_DESCR)) 
	{
	
		if (groupDescr->length != sizeof(MCAST_DESCR))
			;           // Skip over malformed MCAP group address descriptors
		else if (groupDescr->type != MCAST_TYPE)
			;           // Skip over unrecognized descriptor types
		else if ((arb = getMulticastArb(groupDescr->groupAddress)) == NULL)
			;           // Ignore if not in our multicast cache */
		else if (mcap->opcode == MCAP_SOLICIT) 
		{
			mcb = 0;
			OSString *channel = OSString::withCString((const char*)&arb->handle.multicast.channel);
			mcb = OSDynamicCast(MCB, mcapState->getObject(channel));
			channel->free();
			if(mcb)
			{
				if (mcb->ownerNodeID == lcb->ownNodeID)   // Do we own the channel?
					txMCAP(lcb, mcb, 0);             // OK, respond to solicitation
			}
		} 
		else if ((groupDescr->channel != DEFAULT_BROADCAST_CHANNEL) 
				&& (groupDescr->channel <= kMaxChannels)) 
		{
			mcb = 0;
			OSString *channel = OSString::withCString((const char*)&groupDescr->channel);
			mcb = OSDynamicCast(MCB, mcapState->getObject(channel));
			channel->free();
			if(not mcb)
				break;
				
			if (groupDescr->expiration < 60) 
			{
				if (mcb->ownerNodeID == mcapSourceID) 
				{
					currentChannel = groupDescr->channel;
				//	acquireChannel(&currentChannel, TRUE, kDoNotAllocate | kNotifyOnSuccess);
					mcb->ownerNodeID = lcb->ownNodeID;  // Take channel ownership
					mcb->nextTransmit = 1;        // Transmit advertisement ASAP
					
				}
			
			} 
			else if (mcb->ownerNodeID == mcapSourceID) 
			{
				mcb->expiration = groupDescr->expiration;
			}
			else if (mcb->ownerNodeID < mcapSourceID || mcb->expiration < 60) 
			{
            	if (mcb->ownerNodeID == lcb->ownNodeID)   // Are we the owner?
				{
					// releaseChannel(groupDescr->channel, kDoNotDeallocate);
					// TNFReleaseChannel(lcb->unspecifiedDeviceID, groupDescr->channel, kDoNotDeallocate);
				}
				
				mcb->ownerNodeID = mcapSourceID;
				mcb->expiration = groupDescr->expiration;
			}
			currentChannel = arb->handle.multicast.channel;
         
			if (currentChannel == DEFAULT_BROADCAST_CHANNEL) 
			{
				if (mcb->asyncStreamID == kInvalidAsyncStreamRefID) 
				{
					if(groupDescr->channel != DEFAULT_BROADCAST_CHANNEL)
					{
						asyncStreamRxClient = new IOFWAsyncStreamRxCommand;
						if(asyncStreamRxClient == NULL) 
						{
							ioStat = kIOReturnNoMemory;
						}
				
						if(asyncStreamRxClient->initAll(groupDescr->channel, rxAsyncStream, fControl, 
																	fMaxRxIsocPacketSize, this) == false) {
							ioStat = kIOReturnNoMemory;
						}
						if(ioStat == kIOReturnSuccess)
							mcb->asyncStreamID = (UInt32)asyncStreamRxClient;
					}
					else
					{
						if(fBroadcastReceiveClient != NULL)
						{
							fBroadcastReceiveClient->retain();
							mcb->asyncStreamID = (UInt32)fBroadcastReceiveClient;
						}
					}
				}
				
				arb->handle.multicast.channel = groupDescr->channel;
				mcb->groupCount++;
				
			} else if (currentChannel != groupDescr->channel) {
				
				priorMcb = 0;
				OSString *channel = OSString::withCString((const char*)&currentChannel);
				priorMcb = OSDynamicCast(MCB, mcapState->getObject(channel));
				channel->free();
				if(not priorMcb)
					break;
				
				if (priorMcb->groupCount == 1)
				{   
					// Are we the last user?
					asyncStreamRxClient = (IOFWAsyncStreamRxCommand *)mcb->asyncStreamID;
					if(asyncStreamRxClient != NULL)
						asyncStreamRxClient->release();
					//TNFRemoveAsyncStreamClient(lcb->clientID, mcb->asyncStreamID);
					priorMcb->asyncStreamID = kInvalidAsyncStreamRefID;
					priorMcb->groupCount = 0;
				} else if (priorMcb->groupCount > 0)
					priorMcb->groupCount--;
					
				if (mcb->asyncStreamID == kInvalidAsyncStreamRefID) 
				{
					if(groupDescr->channel != DEFAULT_BROADCAST_CHANNEL)
					{				
						asyncStreamRxClient = new IOFWAsyncStreamRxCommand;
						if(asyncStreamRxClient == NULL) 
							ioStat = kIOReturnNoMemory;
				
						if(asyncStreamRxClient->initAll(groupDescr->channel, rxAsyncStream, fControl, 
																			fMaxRxIsocPacketSize, this) == false) 
							ioStat = kIOReturnNoMemory;
							
						if(ioStat == kIOReturnSuccess)
							mcb->asyncStreamID = (UInt32)asyncStreamRxClient;
					}
					else
					{
						if(fBroadcastReceiveClient != NULL)
						{
							fBroadcastReceiveClient->retain();
							mcb->asyncStreamID = (UInt32)fBroadcastReceiveClient;
						}
					}
				}
				
				arb->handle.multicast.channel = groupDescr->channel;
				mcb->groupCount++;
			}
			
			if (mcb->ownerNodeID != lcb->ownNodeID) 
			{
				multicastArb->removeObject(arb);
				arb->release();
			}
		}
		dataSize -= MIN(groupDescr->length, dataSize);
		groupDescr = (MCAST_DESCR*)((ULONG) groupDescr + groupDescr->length);
	}
}

/*!
	@function rxIP
	@abstract Receive IP packet.
	@param pkt - points to the IP packet without the header.
	@param len - length of the packet.
	@params flags - indicates broadcast or unicast	
	@params type - indicates type of the packet IPv4 or IPv6	
	@result IOReturn.
*/
IOReturn IOFWIPBusInterface::rxIP(void *pkt, UInt32 len, UInt32 flags, UInt16 type)
{
	mbuf_t	rxMBuf = NULL;
	struct	firewire_header *fwh = NULL;
	bool	queuePkt = false; 
	IOReturn ret = kIOReturnSuccess;

    IORecursiveLockLock(fIPLock);
    
	if ((rxMBuf = (mbuf_t)allocateMbuf(len)) != NULL) 
	{
        bufferToMbuf(rxMBuf, 0, (UInt8*)pkt, len); 			

		mbuf_prepend(&rxMBuf, sizeof(struct firewire_header), M_DONTWAIT);

        if (rxMBuf != NULL)
        {
			fwh = (struct firewire_header *)mbuf_data(rxMBuf);
            bzero(fwh, sizeof(struct firewire_header));
            fwh->fw_type = htons(type);
			
			queuePkt = (flags == FW_M_UCAST);
			
            if(queuePkt)
				bcopy(fIPLocalNode->macAddr, fwh->fw_dhost, FIREWIRE_ADDR_LEN);
			else
				bcopy(fwbroadcastaddr, fwh->fw_dhost, FIREWIRE_ADDR_LEN);

            if(FWTYPE_IPV6 == type)
                updateNDPCache(rxMBuf);
        }
        else
		{
			fIPLocalNode->fNoMbufs++;
            ret = kIOReturnNoMemory;
		}
		
        if(ret == kIOReturnSuccess)
        {
            fIPLocalNode->receivePackets(rxMBuf, mbuf_pkthdr_len(rxMBuf), queuePkt);
        }
        else
        {
            if(rxMBuf != NULL)
                fIPLocalNode->freePacket((struct mbuf*)rxMBuf, 0);

            fIPLocalNode->networkStatAdd(&(fIPLocalNode->getNetStats())->inputErrors);
        }
	}

    IORecursiveLockUnlock(fIPLock);

	return ret;
}

/*!
	@function rxARP
	@abstract ARP processing routine called from both Asynstream path and Async path.
	@param fwIPObj - IOFireWireIP object.
	@param arp - 1394 arp packet without the GASP or Async header.
	@params flags - indicates broadcast or unicast	
	@result IOReturn.
*/
IOReturn IOFWIPBusInterface::rxARP(IP1394_ARP *arp, UInt32 flags){

	mbuf_t rxMBuf;
	struct firewire_header *fwh = NULL;
	void	*datagram = NULL;
	
	if (arp->hardwareType != htons(ARP_HDW_TYPE)
		|| arp->protocolType != htons(FWTYPE_IP)
		|| arp->hwAddrLen != sizeof(IP1394_HDW_ADDR)
		|| arp->ipAddrLen != IPV4_ADDR_SIZE)
	{
		IOLog("IOFireWireIP: rxARP ERROR in packet header\n");
		return kIOReturnError;
	}

    IORecursiveLockLock(fIPLock);

	if ((rxMBuf = (mbuf_t)allocateMbuf(sizeof(*arp) + sizeof(struct firewire_header))) != NULL) 
	{
		fwh = (struct firewire_header *)mbuf_data(rxMBuf);
		datagram = ((UInt8*)mbuf_data(rxMBuf)) + sizeof(struct firewire_header);
		bzero(fwh, sizeof(struct firewire_header));
		fwh->fw_type = htons(FWTYPE_ARP);
		// Copy the data
		memcpy(datagram, arp, sizeof(*arp));
		
        fIPLocalNode->receivePackets(rxMBuf, mbuf_pkthdr_len(rxMBuf), 0);
	}
	else
		fIPLocalNode->fNoMbufs++;
	
    IORecursiveLockUnlock(fIPLock);
   
 	return kIOReturnSuccess;
}

/*!
	@function watchdog
	@abstract cleans the Link control block's stale drb's and rcb's.
			The cleanCache's job is to age (and eventually discard) device objects 
			for FireWireIP devices that have come unplugged. If they do reappear after  
			they have been discarded from the caches, all that is required is a new ARP. 
			The IP network stack handles that automatically
	@param lcb - the firewire link control block for this interface.
	@result void.
*/
void watchdog(OSObject *obj, IOTimerEventSource *src)
{	
	IOFWIPBusInterface *FWIPPriv = (IOFWIPBusInterface*)obj;

	FWIPPriv->processWatchDogTimeout();
}

void IOFWIPBusInterface::processWatchDogTimeout()
{
	recursiveScopeLock lock(fIPLock);

	updateMcapState();
	
	cleanRCBCache();
	
	// Drop packets based on queue size
	fIPLocalNode->fMaxQueueSize = max(fIPLocalNode->fTxUni - fPrevTransmitCount, TRANSMIT_QUEUE_SIZE);
	
	fPrevTransmitCount = fIPLocalNode->fTxUni;

	// Counter to track the activity of IPoFW layer
	fIPLocalNode->fLastStarted++;
	
	// Tuning segment for optimum performance, if too many Busy Acks
	if( not fIPLocalNode->fDoFastRetry )
	{
		fIPLocalNode->fDoFastRetry	= ((fIPLocalNode->fBusyAcks - fPrevBusyAcks) > kMaxBusyXAcksPerSecond);
		fPrevBusyAcks				= fIPLocalNode->fBusyAcks;
		fFastRetryUnsetTimer		= fPrevFastRetryBusyAcks = fIPLocalNode->fFastRetryBusyAcks = 0;
	}
	else
	{
		fFastRetryUnsetTimer = (fIPLocalNode->fFastRetryBusyAcks - fPrevFastRetryBusyAcks) ? 0 : fFastRetryUnsetTimer + 1;
		
		// Fast retry BusyX acks absent for last 60 seconds, so turn it off
		fIPLocalNode->fDoFastRetry	= not (fFastRetryUnsetTimer > kMaxSecondsToTurnOffFastRetry);
		fPrevBusyAcks				= fIPLocalNode->fBusyAcks;
		fPrevFastRetryBusyAcks		= fIPLocalNode->fFastRetryBusyAcks;
	}
		
	// Restart the watchdog timer
	timerSource->setTimeoutMS(WATCHDOG_TIMER_MS);
}

#pragma mark -
#pragma mark ��� IPv6 NDP routines  ���

const int ipv6fwoffset = 8;

bool IOFWIPBusInterface::addNDPOptions(mbuf_t m)
{
	if(not (mbuf_flags(m) & M_PKTHDR))
		return false;

	vm_address_t src = (vm_offset_t)mbuf_data(m);
	if(src == 0)
		return false;

	UInt32	fwhdrlen	= sizeof(firewire_header);
	mbuf_t	ipv6Mbuf	= m;
	int		pkthdrlen	= 0;

	// check whether len equals ether header
	if(mbuf_len(m) == sizeof(firewire_header))
	{
		ipv6Mbuf = mbuf_next(m);
		if(ipv6Mbuf == NULL)
			return false;

		src = (vm_offset_t)mbuf_data(ipv6Mbuf);
		
		fwhdrlen	= 0;
        pkthdrlen	= mbuf_pkthdr_len(ipv6Mbuf);
	}

	if(mbuf_len(ipv6Mbuf) < (fwhdrlen + sizeof(struct ip6_hdr)))
		return false; 

	// no space in mbuf
	if(mbuf_trailingspace(ipv6Mbuf) < (int)sizeof(IP1394_NDP))
		return false;

	UInt8	*bufPtr = (UInt8*)(src + fwhdrlen);

	// show type of ICMPV6 packets being sent
	struct ip6_hdr				*ip6	= (struct ip6_hdr*)bufPtr;
	struct icmp6_hdr			*icp	= (struct icmp6_hdr*)(ip6 + 1);
	struct nd_neighbor_advert	*nd_na	= (struct nd_neighbor_advert*)icp;
	struct nd_neighbor_solicit	*nd_ns	= (struct nd_neighbor_solicit*)icp;

    int	offset	= sizeof(*ip6) + fwhdrlen;

	bool		modify		 = false;
	IP1394_NDP	*fwndp		 = NULL;
	u_int16_t	*icmp6_cksum = NULL;

	if(nd_ns->nd_ns_type == ND_NEIGHBOR_SOLICIT)
	{		
		// neighbor solicitation
		fwndp = (IP1394_NDP*)((UInt8*)nd_ns + sizeof(struct nd_neighbor_solicit));
		if(fwndp->type == 1)
		{
			modify = true;
            icmp6_cksum = &nd_ns->nd_ns_cksum;
			/*
			IOLog("+type = %d | +len = %d | +srclladdr = %02x:%02x:%02x:%02x:%02x:%02x:%02x::%02x\n", 
				fwndp->type, fwndp->len, fwndp->lladdr[0], fwndp->lladdr[1], fwndp->lladdr[2],
				fwndp->lladdr[3], fwndp->lladdr[4], fwndp->lladdr[5], fwndp->lladdr[6], fwndp->lladdr[7]);
			*/
		}
	}
	
	if(nd_na->nd_na_type == ND_NEIGHBOR_ADVERT)
	{
		// neighbor advertisment
		fwndp =  (IP1394_NDP*)((UInt8*)nd_na + sizeof(struct nd_neighbor_advert));
		
		if(fwndp->type == 2)
		{
			modify = true;
            icmp6_cksum = &nd_na->nd_na_cksum;
			/*
			IOLog("+type = %d | +len = %d | +tgtlladdr = %02x:%02x:%02x:%02x:%02x:%02x:%02x::%02x\n", 
				fwndp->type, fwndp->len, fwndp->lladdr[0], fwndp->lladdr[1], fwndp->lladdr[2],
				fwndp->lladdr[3], fwndp->lladdr[4], fwndp->lladdr[5], fwndp->lladdr[6], fwndp->lladdr[7]);
			*/
		}
	}
	
	if(modify)
	{
		fwndp->len = 3;       									// len in units of 8 octets
		bzero(fwndp->reserved, 6);								// reserved by the RFC 3146
		fwndp->senderMaxRec = fLcb->ownHardwareAddress.maxRec;	// Maximum payload (2 ** senderMaxRec)
		fwndp->sspd = fLcb->ownHardwareAddress.spd;				// Maximum speed
		fwndp->senderUnicastFifoHi = htons(fLcb->ownHardwareAddress.unicastFifoHi);	// Most significant 16 bits of FIFO address
		fwndp->senderUnicastFifoLo = htonl(fLcb->ownHardwareAddress.unicastFifoLo);	// Least significant 32 bits of FIFO address
		
        // current mbuf length IPv6+ICMP6
		mbuf_setlen(ipv6Mbuf, mbuf_len(ipv6Mbuf)+ipv6fwoffset);
		
        // main mbuf header length of FW+IPv6+ICMP6
		mbuf_pkthdr_setlen(m, mbuf_pkthdr_len(m)+ipv6fwoffset);
        
        // fix for <rdar://problem/3483512>: Developer: FireWire IPv6 header payload legnth value 0x28 may be incorrect
        // mbuf header length of IPv6+ICMP6
        if(pkthdrlen != 0)
            mbuf_pkthdr_setlen(ipv6Mbuf, pkthdrlen+ipv6fwoffset);

        int	icmp6len = ntohs(ip6->ip6_plen) + ipv6fwoffset;

        ip6->ip6_plen = htons(icmp6len);
        
        *icmp6_cksum = in6_cksum((struct mbuf*)ipv6Mbuf, IPPROTO_ICMPV6, offset, icmp6len);
		
		// IOLog("ANO: +len = %d ip6_plen %d | csum: %x ip6Size : %d\n", mbuf_len(ipv6Mbuf), ntohs(ip6->ip6_plen), *icmp6_cksum, sizeof(*ip6)); 
		return true;
	}

	return false;
}

void IOFWIPBusInterface::updateNDPCache(mbuf_t m)
{
	if(not (mbuf_flags(m) & M_PKTHDR))
		return ;

	mbuf_t ipv6Mbuf = m;
	int fwhdrlen	= sizeof(firewire_header);
    int pkthdrlen	= 0;
	
	// check whether len equals ether header
	if(mbuf_len(m) == sizeof(firewire_header))
	{
		ipv6Mbuf = mbuf_next(m);
		if(ipv6Mbuf == NULL)
			return;

		fwhdrlen = 0;
        pkthdrlen	= mbuf_pkthdr_len(ipv6Mbuf);
	}

	vm_address_t src = (vm_offset_t)mbuf_data(ipv6Mbuf);
	if(src == 0)
		return;

	if(mbuf_len(ipv6Mbuf) < (sizeof(struct ip6_hdr) + fwhdrlen))
		return;

	// no space in mbuf
	if(mbuf_trailingspace(ipv6Mbuf) < (int)sizeof(IP1394_NDP))
		return;

	// show type of ICMPV6 packets being sent
	struct ip6_hdr		*ip6		= (struct ip6_hdr*)((UInt8*)src + fwhdrlen);
	struct icmp6_hdr	*icp		= (struct icmp6_hdr*)(ip6 + 1);
	struct nd_neighbor_advert	*nd_na	= (struct nd_neighbor_advert*)icp;
	struct nd_neighbor_solicit	*nd_ns	= (struct nd_neighbor_solicit*)icp;

	int		offset	= sizeof(*ip6) + fwhdrlen;

	IP1394_NDP	*fwndp	= NULL;
	bool		modify  = false;

    u_int16_t	*icmp6_cksum = NULL;

	if(nd_ns->nd_ns_type == ND_NEIGHBOR_SOLICIT)
	{		
		// neighbor solicitation
		fwndp = (IP1394_NDP*)((UInt8*)nd_ns + sizeof(struct nd_neighbor_solicit));
		if(fwndp->type == 1)
		{
			modify = true;
            icmp6_cksum = &nd_ns->nd_ns_cksum;
			/*
			IOLog("+type = %d | +len = %d | +srclladdr = %02x:%02x:%02x:%02x:%02x:%02x:%02x::%02x\n", 
				fwndp->type, fwndp->len, fwndp->lladdr[0], fwndp->lladdr[1], fwndp->lladdr[2],
				fwndp->lladdr[3], fwndp->lladdr[4], fwndp->lladdr[5], fwndp->lladdr[6], fwndp->lladdr[7]);
			*/
		}
	}
	
	if(nd_na->nd_na_type == ND_NEIGHBOR_ADVERT)
	{
		// neighbor advertisment
		fwndp =  (IP1394_NDP*)((UInt8*)nd_na + sizeof(struct nd_neighbor_advert));
		
		if(fwndp->type == 2)
		{
			modify = true;
            icmp6_cksum = &nd_na->nd_na_cksum;
			/*
			IOLog("+type = %d | +len = %d | +tgtlladdr = %02x:%02x:%02x:%02x:%02x:%02x:%02x::%02x\n", 
				fwndp->type, fwndp->len, fwndp->lladdr[0], fwndp->lladdr[1], fwndp->lladdr[2],
				fwndp->lladdr[3], fwndp->lladdr[4], fwndp->lladdr[5], fwndp->lladdr[6], fwndp->lladdr[7]);
			*/
		}
	}
    
	ARB			*arb	= NULL;
	
	if(modify && fwndp != NULL && fwndp->len > 2)
	{
		arb = getArbFromFwAddr(fwndp->lladdr);
		
		if(arb != NULL)
		{
			bcopy(fwndp->lladdr, &arb->eui64, FIREWIRE_ADDR_LEN);
			arb->eui64.hi = OSSwapHostToBigInt32(arb->eui64.hi);
			arb->eui64.lo = OSSwapHostToBigInt32(arb->eui64.lo);            
			bcopy(fwndp->lladdr, arb->fwaddr, FIREWIRE_ADDR_LEN);
			arb->handle.unicast.maxRec = fwndp->senderMaxRec;
			arb->handle.unicast.spd = fwndp->sspd;
			arb->handle.unicast.unicastFifoHi = htons(fwndp->senderUnicastFifoHi);
			arb->handle.unicast.unicastFifoLo = htonl(fwndp->senderUnicastFifoLo); 
			arb->handle.unicast.deviceID = getDeviceID(arb->eui64, &arb->itsMac);

			// Reset the packet
			fwndp->len = 2;       	// len in units of 8 octets
			fwndp->senderMaxRec = 0;
			fwndp->sspd = 0;
			fwndp->senderUnicastFifoHi = 0;
			fwndp->senderUnicastFifoLo = 0;
            
            // current mbuf length IPv6+ICMP6
			mbuf_setlen(ipv6Mbuf, mbuf_len(ipv6Mbuf)-8);
			
            // main mbuf header length of FW+IPv6+ICMP6
            mbuf_pkthdr_setlen(m, mbuf_pkthdr_len(m)-8);
            
            // fix for <rdar://problem/3483512>: Developer: FireWire IPv6 header payload legnth value 0x28 may be incorrect
            // mbuf header length of IPv6+ICMP6
            if(pkthdrlen != 0)
				mbuf_pkthdr_setlen(ipv6Mbuf, mbuf_pkthdr_len(ipv6Mbuf)-8);
    
			int	icmp6len = ntohs(ip6->ip6_plen) - 8;
    
            ip6->ip6_plen = htons(icmp6len);
            
            *icmp6_cksum = 0;
            *icmp6_cksum = in6_cksum((struct mbuf*)ipv6Mbuf, IPPROTO_ICMPV6, offset, icmp6len);
		}
	}
}

void IOFWIPBusInterface::updateNDPCache(void *buf, UInt16	*len)
{
	struct icmp6_hdr			*icp	= NULL;
	struct ip6_hdr				*ip6;
	struct nd_neighbor_advert	*nd_na	= NULL;
	struct nd_neighbor_solicit	*nd_ns	= NULL;
	
	ARB			*arb	= NULL;
	IP1394_NDP	*fwndp	= NULL;
	BOOLEAN		update  = false;
	
	ip6		= (struct ip6_hdr*)buf;
	icp		= (struct icmp6_hdr*)(ip6 + 1);
	nd_na	= (struct nd_neighbor_advert*)icp;
	nd_ns	= (struct nd_neighbor_solicit*)icp;

	if(nd_ns->nd_ns_type == ND_NEIGHBOR_SOLICIT)
	{		
		// neighbor solicitation
		fwndp = (IP1394_NDP*)((UInt8*)nd_ns + sizeof(struct nd_neighbor_solicit));
		if(fwndp->type == 1)
		{
			update = true;
			/*
			IOLog("+type = %d | +len = %d | +srclladdr = %02x:%02x:%02x:%02x:%02x:%02x:%02x::%02x\n", 
				fwndp->type, fwndp->len, fwndp->lladdr[0], fwndp->lladdr[1], fwndp->lladdr[2],
				fwndp->lladdr[3], fwndp->lladdr[4], fwndp->lladdr[5], fwndp->lladdr[6], fwndp->lladdr[7]);
			*/
		}
	}
	
	if(nd_na->nd_na_type == ND_NEIGHBOR_ADVERT)
	{
		// neighbor advertisment
		fwndp =  (IP1394_NDP*)((UInt8*)nd_na + sizeof(struct nd_neighbor_advert));
		if(fwndp->type == 2)
		{
			update = true;
			/*
			IOLog("+type = %d | +len = %d | +tgtlladdr = %02x:%02x:%02x:%02x:%02x:%02x:%02x::%02x\n", 
				fwndp->type, fwndp->len, fwndp->lladdr[0], fwndp->lladdr[1], fwndp->lladdr[2],
				fwndp->lladdr[3], fwndp->lladdr[4], fwndp->lladdr[5], fwndp->lladdr[6], fwndp->lladdr[7]);
			*/
		}
	}
	
	if(update && fwndp != NULL && fwndp->len > 2)
	{
		arb = getArbFromFwAddr(fwndp->lladdr);
		
		if(arb != NULL)
		{
			bcopy(fwndp->lladdr, &arb->eui64, FIREWIRE_ADDR_LEN);
			arb->eui64.hi = OSSwapHostToBigInt32(arb->eui64.hi);
			arb->eui64.lo = OSSwapHostToBigInt32(arb->eui64.lo);            
			bcopy(fwndp->lladdr, arb->fwaddr, FIREWIRE_ADDR_LEN);
			arb->handle.unicast.maxRec = fwndp->senderMaxRec;
			arb->handle.unicast.spd = fwndp->sspd;
			arb->handle.unicast.unicastFifoHi = htons(fwndp->senderUnicastFifoHi);
			arb->handle.unicast.unicastFifoLo = htonl(fwndp->senderUnicastFifoLo); 
			arb->handle.unicast.deviceID = getDeviceID(arb->eui64, &arb->itsMac);

			// Reset the packet
			*len -= 8;
			fwndp->len = 2;       	// len in units of 8 octets
			fwndp->senderMaxRec = 0;
			fwndp->sspd = 0;
			fwndp->senderUnicastFifoHi = 0;
			fwndp->senderUnicastFifoLo = 0;
		}
	}

	return;
}

#pragma mark -
#pragma mark ��� IOFWIPBusInterface utility routines  ���

bool IOFWIPBusInterface::staticUpdateARPCache(void *refcon, IP1394_ARP *fwa)
{
	return ((IOFWIPBusInterface*)refcon)->updateARPCache(fwa);
}

UInt32	IOFWIPBusInterface::staticOutputPacket(mbuf_t pkt, void * param)
{
	return ((IOFWIPBusInterface*)param)->outputPacket(pkt,param);
}

/*!
	@function updateARPCache
	@abstract updates IPv4 ARP cache from the incoming ARP packet 
	@param fwa - firewire ARP packet.
	@result void.
*/
bool IOFWIPBusInterface::updateARPCache(IP1394_ARP *fwa)
{
    ARB		*fwarb	= NULL;
	UWIDE	eui64;
	
    IORecursiveLockLock(fIPLock);

	eui64.hi = htonl(fwa->senderUniqueID.hi);	
	eui64.lo = htonl(fwa->senderUniqueID.lo);

	// Get the arb pointer from sdl->data
	fwarb = getARBFromEui64(eui64);

	if(fwarb)
	{
		fwarb->handle.unicast.maxRec = fwa->senderMaxRec; // Volatile fields
		fwarb->handle.unicast.spd = fwa->sspd;
		fwarb->handle.unicast.unicastFifoHi = htons(fwa->senderUnicastFifoHi);
		fwarb->handle.unicast.unicastFifoLo = htonl(fwa->senderUnicastFifoLo);
		fwarb->eui64.hi = eui64.hi;	
		fwarb->eui64.lo = eui64.lo;
		fwarb->handle.unicast.deviceID = getDeviceID(fwarb->eui64, &fwarb->itsMac);    

		fIPLocalNode->getBytesFromGUID(&fwarb->eui64, fwarb->fwaddr, 0);
	}
	
	IORecursiveLockUnlock(fIPLock);
	
	return true;
}

ARB *IOFWIPBusInterface::updateARBwithDevice(IOFireWireNub *device, UWIDE eui64)
{
    IORecursiveLockLock(fIPLock);

	// Create the arb if we recognise a IP unit.
	ARB *arb = getARBFromEui64(eui64);

	// Update the device object in the address resolution block used in the ARP resolve routine
	if(arb != NULL)
	{
		arb->handle.unicast.deviceID	= (UInt32)device;
		arb->handle.unicast.maxRec		= device->maxPackLog(true); 
		arb->handle.unicast.spd			= device->FWSpeed();
		arb->itsMac = false;
		arb->eui64.hi = eui64.hi;	
		arb->eui64.lo = eui64.lo;
		fIPLocalNode->getBytesFromGUID(&eui64, arb->fwaddr, 0);
	}

	IORecursiveLockUnlock(fIPLock);

	return arb;
}

/*!
	@function cleanFWRcbCache
	@abstract cleans the Link control block's stale rcb's. UnAssembled RCB's
				are returned to the free CBLKs
	@param none.
	@result void.
*/
void IOFWIPBusInterface::cleanRCBCache()
{
    IORecursiveLockLock(fIPLock);

	RCB *rcb = 0;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( activeRcb );
	while( NULL != (rcb = OSDynamicCast(RCB, iterator->getNextObject())) )
	{
		if(rcb->timer > 1)
			rcb->timer--; // still reassembling packets
		else if (rcb->timer == 1) 
			releaseRCB(rcb);
    }
	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);
}

/*!
	@function getDeviceID
	@abstract returns a fireWire device object for the GUID
	@param lcb - the firewire link control block for this interface.
    @param eui64 - global unique id of a device on the bus.
    @param itsMac - destination is Mac or not.
	@result Returns IOFireWireNub if successfull else 0.
*/
UInt32 IOFWIPBusInterface::getDeviceID(UWIDE eui64, BOOLEAN *itsMac) {  

    // Returns DRB if EUI-64 matches
    DRB *drb = getDrbFromEui64(eui64);   

    // Device reference ID already created
    if (drb != NULL) 
	{              
		*itsMac = drb->itsMac;
        // Just return it to caller
        return(drb->deviceID);        
    }
    else 
	{
		*itsMac = false;
        // Get an empty DRB
        return(kInvalidIPDeviceRefID);
    }
}

void IOFWIPBusInterface::releaseDRB(u_char *fwaddr)
{
    IORecursiveLockLock(fIPLock);

	DRB *drb = NULL;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( activeDrb );
	while( NULL != (drb = OSDynamicCast(DRB, iterator->getNextObject())) )
	{
		if (bcmp(fwaddr, drb->fwaddr, FIREWIRE_ADDR_LEN) == 0)
		{
			drb->deviceID = kInvalidIPDeviceRefID;  // Don't notify in future
            activeDrb->removeObject(drb);			// time to clean up
			drb->release();
		}
	}
	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);
}

void IOFWIPBusInterface::releaseDRB(ULONG deviceID)
{
    IORecursiveLockLock(fIPLock);

	DRB *drb = NULL;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( activeDrb );
	while( NULL != (drb = OSDynamicCast(DRB, iterator->getNextObject())) )
	{
		if (drb->deviceID == deviceID) 
		{
			drb->deviceID = kInvalidIPDeviceRefID;  // Don't notify in future
            activeDrb->removeObject(drb);			// time to clean up
			drb->release();
			break;
		}
	}
	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);
}

void IOFWIPBusInterface::releaseARB(u_char *fwaddr)
{
    IORecursiveLockLock(fIPLock);

	ARB *arb = NULL;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( unicastArb );

	while( NULL != (arb = OSDynamicCast(ARB, iterator->getNextObject())) )
	{
		if (bcmp(fwaddr, arb->fwaddr, FIREWIRE_ADDR_LEN) == 0)
		{
			arb->handle.unicast.deviceID = kInvalidIPDeviceRefID;
			unicastArb->removeObject(arb);
			arb->release();
		}
	}
					
	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);
}

void IOFWIPBusInterface::releaseRCB(RCB *rcb, bool freeMbuf)
{
    IORecursiveLockLock(fIPLock);

	if(freeMbuf && rcb->mBuf != NULL)
	{
		fIPLocalNode->freePacket((struct mbuf*)rcb->mBuf, 0);
		rcb->mBuf = NULL;
	}
	activeRcb->removeObject(rcb);
	fRCBCmdPool->returnCommand(rcb);

    IORecursiveLockUnlock(fIPLock);
}   

void IOFWIPBusInterface::updateMcapState()
{
	IOFWAsyncStreamRxCommand *asyncStreamRxClient;

    IORecursiveLockLock(fIPLock);

	MCB	*mcb = NULL;
	OSCollectionIterator	*iterator = OSCollectionIterator::withCollection( mcapState );
	while( NULL != (mcb = OSDynamicCast(MCB, iterator->getNextObject())) )
	{
		if (mcb->expiration > 1)		// Life in this channel allocation yet?
			mcb->expiration--;			// Yes, but the clock is ticking...
		else if (mcb->expiration == 1)	// Dead in the water?
		{ 
			mcb->expiration = 0;        // Yes, mark it expired
			asyncStreamRxClient = (IOFWAsyncStreamRxCommand *)mcb->asyncStreamID;
			if(asyncStreamRxClient != NULL)
				asyncStreamRxClient->release();
			mcb->asyncStreamID = kInvalidAsyncStreamRefID;
			if (mcb->ownerNodeID == fLcb->ownNodeID) // We own the channel?
			{  
				mcb->finalWarning = 4;  // Yes, four final advertisements
				mcb->nextTransmit = 1;  // Starting right now... 
			}
		}
		if (mcb->ownerNodeID != fLcb->ownNodeID)
			continue;                     // Cycle to next array entry 
		else if (mcb->nextTransmit > 1)  // Time left before next transmit? 
			mcb->nextTransmit--;                         // Keep on ticking... 
		else if (mcb->nextTransmit == 1) 
		{              // Due to expire now? 
			if (mcb->groupCount > 0)      // Still in use at this machine? 
				mcb->expiration = 60;      // Renew this channel's lease
				
			txMCAP(fLcb, mcb, 0);          // Broadcast the MCAP advertisement
			
			if (mcb->expiration > 0)
				mcb->nextTransmit = 10;    // Send MCAP again in ten seconds 
			else if (--mcb->finalWarning > 0)
				mcb->nextTransmit = 10;    // Channel deallocation warning 
			else 
			{
				mcb->ownerNodeID = MCAP_UNOWNED; // Reliquish our ownership 
				mcb->nextTransmit = 0;           // We're really, really done! 
				releaseMulticastARB(mcb);
			}
		}
	}
	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);
}

void IOFWIPBusInterface::releaseMulticastARB(MCB *mcb)
{
    IORecursiveLockLock(fIPLock);
	
	ARB *arb = NULL;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( multicastArb );
	while( NULL != (arb = OSDynamicCast(ARB, iterator->getNextObject())) )
	{
		if (arb->handle.multicast.channel == mcb->channel)
		{
			multicastArb->removeObject(arb); 
			arb->release();
			continue;
		}
	}
	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);
}

void IOFWIPBusInterface::resetRCBCache()
{
	IORecursiveLockLock(fIPLock);

	RCB	*rcb = NULL;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( activeRcb );
	while( NULL != (rcb = OSDynamicCast(RCB, iterator->getNextObject())) )
		releaseRCB(rcb);

	iterator->release();

	IORecursiveLockUnlock(fIPLock);
}

void IOFWIPBusInterface::resetARBCache()
{
	IORecursiveLockLock(fIPLock);

	ARB *arb = NULL;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( unicastArb );
	while( NULL != (arb = OSDynamicCast(ARB, iterator->getNextObject())) )
		arb->handle.unicast.deviceID = kInvalidIPDeviceRefID;

	iterator->release();

	IORecursiveLockUnlock(fIPLock);
}

void IOFWIPBusInterface::resetMcapState()
{
	IORecursiveLockLock(fIPLock);
	
	MCB	*mcb = NULL;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( mcapState );

	while( NULL != (mcb = OSDynamicCast(MCB, iterator->getNextObject())) )
		mcb->nextTransmit = 0;
		
	iterator->release();

	IORecursiveLockUnlock(fIPLock);
}

/*! 
	@function getARBFromEui64
	@abstract Locates the corresponding Unicast ARB (Address resolution block) for GUID
	@param lcb - the firewire link control block for this interface.
	@param eui64 - global unique id of a device on the bus.
	@result Returns ARB if successfull else NULL.
*/
ARB *IOFWIPBusInterface::getARBFromEui64(UWIDE eui64) 
{  
    IORecursiveLockLock(fIPLock);

	ARB *arb = 0;
	OSCollectionIterator *iterator = OSCollectionIterator::withCollection( unicastArb );
   
   	while( NULL != (arb = OSDynamicCast(ARB, iterator->getNextObject())) )
        if (arb->eui64.hi == eui64.hi && arb->eui64.lo == eui64.lo)
            break;
   
	iterator->release();
	
	if(arb == NULL)
	{
		// Create a new entry if it does not exist
		if((arb = new ARB) == NULL)
			return arb;

		unicastArb->setObject(arb);  
	}
   
    IORecursiveLockUnlock(fIPLock);
	
    return(arb);
}

/*! 
	@function getArbFromFwAddr
	@abstract Locates the corresponding Unicast ARB (Address resolution block) for GUID
	@param lcb - the firewire link control block for this interface.
	@param FwAddr - global unique id of a device on the bus.
	@result Returns ARB if successfull else NULL.
*/
ARB *IOFWIPBusInterface::getArbFromFwAddr(u_char *fwaddr) 
{
	IORecursiveLockLock(fIPLock);

	ARB *arb = 0;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( unicastArb );

	while( NULL != (arb = OSDynamicCast(ARB, iterator->getNextObject())) )
		if (bcmp(fwaddr, arb->fwaddr, FIREWIRE_ADDR_LEN) == 0)
					break;
					
	iterator->release();
	
	IORecursiveLockUnlock(fIPLock);
	
	return(arb);
}

/*!
	@function getDrbFromEui64
	@abstract Locates the corresponding DRB (device reference block) for GUID
	@param lcb - the firewire link control block for this interface.
	@param eui64 - global unique id of a device on the bus.
	@result Returns DRB if successfull else NULL.
*/
DRB *IOFWIPBusInterface::getDrbFromEui64(UWIDE eui64)
{  
    IORecursiveLockLock(fIPLock);

	DRB *drb = 0;
	OSCollectionIterator * iterator = OSCollectionIterator::withCollection( activeDrb );

	while( NULL != (drb = OSDynamicCast(DRB, iterator->getNextObject())) )
        if (drb->eui64.hi == eui64.hi && drb->eui64.lo == eui64.lo)
            break;
   
	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);
   
    return(drb);
}

/*!
	@function getDrbFromFwAddr
	@abstract Locates the corresponding DRB (device reference block) for GUID
	@param lcb - the firewire link control block for this interface.
	@param fwaddr - global unique id of a device on the bus.
	@result Returns DRB if successfull else NULL.
*/
DRB *IOFWIPBusInterface::getDrbFromFwAddr(u_char *fwaddr) 
{  
    IORecursiveLockLock(fIPLock);

	DRB *drb = 0;
	OSCollectionIterator *iterator = OSCollectionIterator::withCollection( activeDrb );
   
   	while( NULL != (drb = OSDynamicCast(DRB, iterator->getNextObject())) )
        if (bcmp(fwaddr, drb->fwaddr, FIREWIRE_ADDR_LEN) == 0)
            break;

	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);
   
    return(drb);
}



/*!
	@function getDrbFromDeviceID
	@abstract Locates the corresponding DRB (Address resolution block) for IOFireWireNub
    @param deviceID - IOFireWireNub to look for.
	@result Returns DRB if successfull else NULL.
*/
DRB *IOFWIPBusInterface::getDrbFromDeviceID(void *deviceID)
{   
    IORecursiveLockLock(fIPLock);

	DRB *drb = 0;
	OSCollectionIterator *iterator = OSCollectionIterator::withCollection( activeDrb );
	
   	while( NULL != (drb = OSDynamicCast(DRB, iterator->getNextObject())) )
        if (drb->deviceID == (UInt32)deviceID)
            break;
			
	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);
   
    return  drb;
}

/*!
	@function getMulticastArb
	@abstract Locates the corresponding multicast MARB (Address resolution block) for ipaddress
	@param lcb - the firewire link control block for this interface.
	@param ipAddress - destination ipaddress to send the multicast packet.
	@result Returns MARB if successfull else NULL.
*/
MARB *IOFWIPBusInterface::getMulticastArb(UInt32 ipAddress)
{  
    IORecursiveLockLock(fIPLock);

	MARB *arb = 0;
	OSCollectionIterator *iterator = OSCollectionIterator::withCollection( multicastArb );
	
   	while( NULL != (arb = OSDynamicCast(MARB, iterator->getNextObject())) )
        if (arb->ipAddress == ipAddress)
            break;

	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);
         
    return arb;
}

/*!
	@function getRcb
	@abstract Locates a reassembly control block.
	@param lcb - the firewire link control block for this interface.
    @param sourceID - source nodeid which generated the fragmented packet.
    @param dgl - datagram label for the fragmented packet.
	@result Returns RCB if successfull else NULL.
*/
RCB *IOFWIPBusInterface::getRcb(UInt16 sourceID, UInt16 dgl)
{
    IORecursiveLockLock(fIPLock);

	RCB *rcb = 0;
	OSCollectionIterator *iterator = OSCollectionIterator::withCollection( activeRcb );
	
   	while( NULL != (rcb = OSDynamicCast(RCB, iterator->getNextObject())) )
        if (rcb->sourceID == sourceID && rcb->dgl == dgl)
            break;

	iterator->release();
	
    IORecursiveLockUnlock(fIPLock);

    return(rcb);
}

RCB *IOFWIPBusInterface::getRCBCommand( UInt16 sourceID, UInt16 dgl, UInt16 etherType, UInt16 datagramSize, mbuf_t m )
{
	RCB * cmd = (RCB *)fRCBCmdPool->getCommand(false);

	if( ( cmd == NULL ) and ( fCurrentRCBCommands < kMaxAsyncCommands ) ) 
	{	
		if( ( cmd = new RCB ) != NULL ) 
			fCurrentRCBCommands++;
	}
	
	if( cmd )
		cmd->reinit( sourceID, dgl, etherType, datagramSize, m );
	
	return cmd;
}

#pragma mark -
#pragma mark ��� Control Block Routines ���

void RCB::reinit(UInt16 id, UInt16 label, UInt16 type, UInt16 size, mbuf_t m)
{
	sourceID		= id;
	dgl				= label;
	mBuf			= m;
	timer			= kRCBExpirationtime;
	datagramSize	= size;
	etherType		= type;
	residual		= 0;
}

void RCB::free()
{
	OSObject::free();
}

#pragma mark -
#pragma mark ��� Mbuf Utility Routines ���

bool IOFWIPMBufCommand::init()
{
	if ( not OSObject::init() )
		return false;

	fMbuf			= NULL;
	fIPLocalNode	= NULL;
	fStatus			= kIOReturnSuccess;

	return true;
}

void IOFWIPMBufCommand::reinit(mbuf_t pkt, IOFireWireIP *ipNode, IOCommandPool *pool)
{
	fIPLocalNode	= ipNode;	
	fMbuf			= pkt;
	fStatus			= kIOReturnSuccess;
	fPool			= pool;
	fInited			= true;
}

mbuf_t IOFWIPMBufCommand::getMBuf()
{
	return fMbuf;
}

void IOFWIPMBufCommand::releaseWithStatus(IOReturn status)
{
	if (status == kIOFireWireOutOfTLabels)
		fStatus = status;
	
	if(this->getRetainCount() == 2)
	{
		if ( fInited )
		{
			if( fMbuf && (fStatus != kIOFireWireOutOfTLabels) )
			{
				fIPLocalNode->inActiveMbufs++;
				fIPLocalNode->freePacket((struct mbuf*)fMbuf);
				fMbuf = NULL;
			}
			fIPLocalNode = NULL;
		}

		fInited = false;

		this->release();
		
		fPool->returnCommand(this);
	}
	else
		this->release();
}

void IOFWIPMBufCommand::free()
{
	fMbuf = NULL;
	fIPLocalNode = NULL;
	fStatus = kIOReturnSuccess;

	OSObject::free();
}

static mbuf_t getPacket( UInt32 size,
                                UInt32 how,
                                UInt32 smask,
                                UInt32 lmask )
{
    mbuf_t packet;
	UInt32 reqSize =  size + smask + lmask; 	// we over-request so we can fulfill alignment needs.
	
	if(reqSize > MHLEN && reqSize <= MINCLSIZE)	//as protection from drivers that incorrectly assume they always get a single-mbuf packet
		reqSize = MINCLSIZE + 1;				//we force kernel to give us a cluster instead of chained small mbufs.

	if( 0 == mbuf_allocpacket(how, reqSize, NULL, &packet))
	{
		mbuf_t m = packet;
		mbuf_pkthdr_setlen(packet, size);
		//run the chain and apply alignment
		
		while(size && m)
		{
			vm_address_t alignedStart, originalStart;
			
			originalStart = (vm_address_t)mbuf_data(m);
			alignedStart = (originalStart + smask) & ~smask;
			mbuf_setdata(m,  (caddr_t)alignedStart, (mbuf_maxlen(m) - (alignedStart - originalStart)) & ~lmask);
			
			if(mbuf_len(m) > size)
				mbuf_setlen(m, size); //truncate to remaining portion of packet

			size -= mbuf_len(m);
			m = mbuf_next(m);
		}
		
		// mbuf_settype(packet, MBUF_TYPE_PCB);
		
		return packet;
	}
	else
		return NULL;
}

mbuf_t IOFWIPBusInterface::allocateMbuf( UInt32 size )
{
    return getPacket( size, M_DONTWAIT, kIOPacketBufferAlign1, kIOPacketBufferAlign16 );
}

void IOFWIPBusInterface::moveMbufWithOffset(SInt32 tempOffset, mbuf_t *srcm, vm_address_t *src, SInt32 *srcLen)
{
    mbuf_t temp = NULL;

	for(;;) 
	{

		if(tempOffset == 0)
			break;

		if(*srcm == NULL)
			break;

		if(*srcLen < tempOffset) 
		{
			tempOffset = tempOffset - *srcLen;
			temp = mbuf_next(*srcm); 
			*srcm = temp;
			if(*srcm != NULL)
				*srcLen = mbuf_len(*srcm);
			continue;
		} 
		else if (*srcLen > tempOffset) 
		{
			*srcLen = mbuf_len(*srcm);
			*src = (vm_offset_t)mbuf_data(*srcm);
			*src += tempOffset;
			*srcLen -= tempOffset;
			break;
		} 
		else if (*srcLen == tempOffset) 
		{
			temp = mbuf_next(*srcm); 
			*srcm = temp;
			if(*srcm != NULL) 
			{
				*srcLen = mbuf_len(*srcm);
				*src = (vm_offset_t)mbuf_data(*srcm);
			}
			break;
		}
	}
}

/*!
	@function bufferToMbuf
	@abstract Copies buffer to Mbuf.
	@param m - destination mbuf.
	@param offset - offset into the mbuf data pointer.
	@param srcbuf - source buf.
	@param srcbufLen - source buffer length.
	@result bool - true if success else false.
*/
bool IOFWIPBusInterface::bufferToMbuf(mbuf_t m, 
								UInt32 offset, 
								UInt8  *srcbuf, 
								UInt32 srcbufLen)
{
    IORecursiveLockLock(fIPLock);

	// Get the source
	mbuf_t srcm = m; 
	SInt32 srcLen = mbuf_len(srcm);
    vm_address_t src = (vm_offset_t)mbuf_data(srcm);

	// Mbuf manipulated to point at the correct offset
	SInt32 tempOffset = offset;

	moveMbufWithOffset(tempOffset, &srcm, &src, &srcLen);

	// Modify according to our fragmentation
	SInt32 dstLen = srcbufLen;
    SInt32 copylen = dstLen;
	vm_address_t dst = (vm_address_t)srcbuf;
	
    mbuf_t temp = NULL;
	
    for (;;) {
	
        if (srcLen < dstLen) {

            // Copy remainder of buffer to current mbuf upto m_len.
            BCOPY(dst, src, srcLen);
            dst += srcLen;
            dstLen -= srcLen;
			copylen -= srcLen;
			// set the offset
			
			if(copylen == 0){
				// set the new mbuf to point to the new chain
				temp = mbuf_next(srcm); 
				srcm = temp;
				break;
			}
            // Move on to the next source mbuf.
            temp = mbuf_next(srcm); assert(temp);
            srcm = temp;
            srcLen = mbuf_len(srcm);
            src = (vm_offset_t)mbuf_data(srcm);
        }
        else if (srcLen > dstLen) {
			//
            // Copy some of buffer to src mbuf, since mbuf 
			// has more space.
			//
            BCOPY(dst, src, dstLen);
            src += dstLen;
            srcLen -= dstLen;
            copylen -= dstLen;
			
			if(copylen == 0)
				break;
        }
        else {  /* (srcLen == dstLen) */
            // copy remainder of src into remaining space of current mbuffer
            BCOPY(dst, src, srcLen);
			copylen -= srcLen;
			
			if(copylen == 0){
				// set the new mbuf to point to the new chain
				temp = mbuf_next(srcm); 
				srcm = temp;
				break;
			}
            // Free current mbuf and move the current onto the next
            srcm = mbuf_next(srcm);

            // Do we have any data left to copy?
            if (dstLen == 0)
				break;

            srcLen = mbuf_len(srcm);
            src = (vm_offset_t)mbuf_data(srcm);
        }
    }
    IORecursiveLockUnlock(fIPLock);
	
	return true;
}

/*!
	@function mbufTobuffer
	@abstract Copies mbuf data into the buffer pointed by IOMemoryDescriptor.
	@param src - source mbuf.
	@param offset - offset into the mbuf data pointer.
	@param dstbuf - destination buf.
	@param dstbufLen - destination buffer length.
	@param length - length to copy.
	@result NULL if copied else should be invoked again till 
			the residual is copied into the buffer.
*/
mbuf_t IOFWIPBusInterface::mbufTobuffer(const mbuf_t m, 
								UInt32 *offset, 
								UInt8  *dstbuf, 
								UInt32 dstbufLen, 
								UInt32 length)
{
	// Get the source
	mbuf_t srcm = m; 
	SInt32 srcLen = mbuf_len(srcm);
    vm_address_t src = (vm_offset_t)mbuf_data(srcm);
	
	// Mbuf manipulated to point at the correct offset
	SInt32 tempOffset = *offset;

	moveMbufWithOffset(tempOffset, &srcm, &src, &srcLen);

	// Modify according to our fragmentation
	SInt32 dstLen = length;
    SInt32 copylen = dstLen;
	vm_address_t dst = (vm_address_t)dstbuf;
	
	mbuf_t temp = NULL;

    for (;;) {
	
        if (srcLen < dstLen) {

            // Copy remainder of src mbuf to current dst.
            BCOPY(src, dst, srcLen);
            dst += srcLen;
            dstLen -= srcLen;
			copylen -= srcLen;
			// set the offset
			*offset = *offset + srcLen; 
			
			if(copylen == 0){
				// set the new mbuf to point to the new chain
				temp = mbuf_next(srcm); 
				srcm = temp;
				break;
			}
            // Move on to the next source mbuf.
            temp = mbuf_next(srcm); assert(temp);
            srcm = temp;
            srcLen = mbuf_len(srcm);
            src = (vm_offset_t)mbuf_data(srcm);
        }
        else if (srcLen > dstLen) {
            // Copy some of src mbuf to remaining space in dst mbuf.
            BCOPY(src, dst, dstLen);
            src += dstLen;
            srcLen -= dstLen;
            copylen -= dstLen;
			// set the offset
			*offset = *offset + dstLen; 

            // Move on to the next destination mbuf.
			if(copylen == 0)
				break;
        }
        else {  /* (srcLen == dstLen) */
            // copy remainder of src into remaining space of current dst
            BCOPY(src, dst, srcLen);
			copylen -= srcLen;
			
			if(copylen == 0){
				// set the offset
				*offset = 0; 
				// set the new mbuf to point to the new chain
				temp = mbuf_next(srcm); 
				srcm = temp;
				break;
			}
            // Free current mbuf and move the current onto the next
            srcm = mbuf_next(srcm);

            // Do we have any data left to copy?
            if (dstLen == 0)
				break;

            srcLen = mbuf_len(srcm);
            src = (vm_offset_t)mbuf_data(srcm);
        }
    }

	return temp;
}

#ifdef DEBUG

#pragma mark -
#pragma mark ��� Debug Routines ���

void IOFWIPBusInterface::showMinRcb(RCB *rcb) {
	if (rcb != NULL) {
		if(rcb->timer == 1)
			IOLog("RCB %p dgl %u mBuf %p datagramSize %u residual %u timer %u \n", rcb, rcb->dgl, rcb->mBuf, rcb->datagramSize,  rcb->residual, rcb->timer);
	}
}

// Display the reassembly control block
void IOFWIPBusInterface::showRcb(RCB *rcb) {
	if (rcb != NULL) {
      IOLog("RCB %p\n\r", rcb);
      IOLog(" sourceID %04X dgl %u etherType %04X mBlk %p\n\r", rcb->sourceID, rcb->dgl, rcb->etherType, rcb->mBuf);
      IOLog(" datagramSize %u residual %u timer %u \n\r", rcb->datagramSize, rcb->residual, rcb->timer);
	}
}

void IOFWIPBusInterface::showArb(ARB *arb) 
{
   IOLog("ARB %p\n\r", arb);

   IOLog(" EUI-64 %08lX %08lX\n\r", arb->eui64.hi, arb->eui64.lo);
   
   IOLog(" fwAddr  %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n\r", arb->fwaddr[0],
          arb->fwaddr[1], arb->fwaddr[2], arb->fwaddr[3], arb->fwaddr[4],
          arb->fwaddr[5], arb->fwaddr[6], arb->fwaddr[7]);

   IOLog(" Handle: %08lX %02X %02X %04X%08lX\n\r", arb->handle.unicast.deviceID,
          arb->handle.unicast.maxRec, arb->handle.unicast.spd,
          arb->handle.unicast.unicastFifoHi, arb->handle.unicast.unicastFifoLo);
}

void IOFWIPBusInterface::showHandle(TNF_HANDLE *handle) {

   if (handle->unicast.deviceID != kInvalidIPDeviceRefID)
      IOLog("   Unicast handle: %08lX %02X %02X %04X%08lX\n\r",
             handle->unicast.deviceID, handle->unicast.maxRec,
             handle->unicast.spd, handle->unicast.unicastFifoHi,
             handle->unicast.unicastFifoLo);
   else
      IOLog("   Multicast handle: 00000000 %02X %02X %02X %08lX\n\r",
             handle->multicast.maxRec, handle->multicast.spd,
             handle->multicast.channel, htonl(handle->multicast.groupAddress));

}

void IOFWIPBusInterface::showDrb(DRB *drb) 
{
   if (drb != NULL) {
      IOLog("DRB 0x%p \n\r", drb);
      IOLog(" Device ID %08lX EUI-64 %08lX %08lX\n\r", drb->deviceID, drb->eui64.hi, drb->eui64.lo);
      IOLog(" maxPayload %d maxSpeed %d\n\r", drb->maxPayload, drb->maxSpeed);
   }
}

void IOFWIPBusInterface::showLcb()
{
	IOLog(" Node ID %04X maxPayload %u maxSpeed %u busGeneration 0x%08lX\n",
		  fLcb->ownNodeID, fLcb->ownMaxPayload,
		  fLcb->ownMaxSpeed, fLcb->busGeneration);

	// Display the arb's
	IORecursiveLockLock(fIPLock);

	OSCollectionIterator * iterator = 0;

	ARB *arb = 0;
	
	iterator = OSCollectionIterator::withCollection( unicastArb );

	IOLog(" Unicast ARBs\n\r");
	while( NULL != (arb = OSDynamicCast(ARB, iterator->getNextObject())) )
	{
		IOLog("  %p\n\r", arb);
		showArb(arb);
	}
					
	iterator->release();

	IOLog(" Active DRBs\n\r");
	DRB *drb = 0;
	iterator = OSCollectionIterator::withCollection( activeDrb );

	while( NULL != (drb = OSDynamicCast(DRB, iterator->getNextObject())) )
	{
		 IOLog("  %p\n\r", drb);
		 showDrb(drb);
	}

	iterator->release();

	RCB *rcb = 0;
	iterator = OSCollectionIterator::withCollection( activeRcb );
	UInt32	rcbCount = 0;
	
	while( NULL != (rcb = OSDynamicCast(RCB, iterator->getNextObject())) )
	{
		 showMinRcb(rcb);
		 rcbCount++;
	}
	IOLog(" Active RCBs %u \n", rcbCount);
	

	iterator->release();

	IORecursiveLockUnlock(fIPLock);
}

#endif