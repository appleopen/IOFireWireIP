/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1999 Apple Computer, Inc.  All rights reserved. 
 *
 * IOFWInterface.cpp
 *
 */

#include <IOKit/assert.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include "IOFWInterface.h"
#include "IOFWController.h"
//#include <IOKit/network/IONetworkUserClient.h>
#include <IOKit/pwr_mgt/RootDomain.h>	// publishFeature()

extern "C" {
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <net/if.h>
#include <firewire.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/dlil.h>
#include <net/bpf.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
//#include <netinet/if_ether.h>
#include <sys/sockio.h>
#include <sys/malloc.h>
}
#include "firewire.h"
#include "if_firewire.h"

#ifndef DLT_APPLE_IP_OVER_IEEE1394
#define DLT_APPLE_IP_OVER_IEEE1394 138
#endif
//---------------------------------------------------------------------------

#define super IONetworkInterface

OSDefineMetaClassAndStructors( IOFWInterface, IONetworkInterface )
OSMetaClassDefineReservedUnused( IOFWInterface,  0);
OSMetaClassDefineReservedUnused( IOFWInterface,  1);
OSMetaClassDefineReservedUnused( IOFWInterface,  2);
OSMetaClassDefineReservedUnused( IOFWInterface,  3);
OSMetaClassDefineReservedUnused( IOFWInterface,  4);
OSMetaClassDefineReservedUnused( IOFWInterface,  5);
OSMetaClassDefineReservedUnused( IOFWInterface,  6);
OSMetaClassDefineReservedUnused( IOFWInterface,  7);
OSMetaClassDefineReservedUnused( IOFWInterface,  8);
OSMetaClassDefineReservedUnused( IOFWInterface,  9);
OSMetaClassDefineReservedUnused( IOFWInterface, 10);
OSMetaClassDefineReservedUnused( IOFWInterface, 11);
OSMetaClassDefineReservedUnused( IOFWInterface, 12);
OSMetaClassDefineReservedUnused( IOFWInterface, 13);
OSMetaClassDefineReservedUnused( IOFWInterface, 14);
OSMetaClassDefineReservedUnused( IOFWInterface, 15);

// The name prefix for all BSD FireWire interfaces.
// 
#define kIOFWInterfaceNamePrefix      "fw"

#define IFP2AC(IFP)		((struct arpcom *)IFP)

// Options used for enableFilter(), disableFilter().
enum {
    kFilterOptionDeferIO          = 0x0001,
    kFilterOptionNotInsideGate    = 0x0002,
    kFilterOptionNoStateChange    = 0x0004,
    kFilterOptionDisableZeroBits  = 0x0008,
    kFilterOptionSyncPendingIO    = 0x0010
};
static u_long ivedonethis = 0;
extern void	firewire_arp_ifinit __P((struct arpcom *, struct ifaddr *));

//---------------------------------------------------------------------------
// Macros

#ifdef  DEBUG
#define DLOG(fmt, args...)  IOLog(fmt, ## args)
#else
#define DLOG(fmt, args...)
#endif

UInt32 IOFWInterface::getFilters(const OSDictionary *dict,
								const OSSymbol		*group)
{
    OSNumber * num;
    UInt32     filters = 0;

    assert( dict && group );

    if (( num = (OSNumber *) dict->getObject(group) ))
    {
        filters = num->unsigned32BitValue();
    }
    return filters;
}

bool IOFWInterface::setFilters(OSDictionary		*dict,
								const OSSymbol	*group,
								UInt32           filters)
{
    OSNumber * num;
    bool       ret = false;

    assert( dict && group );

    num = (OSNumber *) dict->getObject(group);
    if ( num == 0 )
    {
        if (( num = OSNumber::withNumber(filters, 32) ))
        {
            ret = dict->setObject(group, num);
            num->release();
        }
    }
    else
    {
        num->setValue(filters);
        ret = true;
    }
    return ret;
}

#define GET_REQUIRED_FILTERS(g)     getFilters(_requiredFilters, (g))
#define GET_ACTIVE_FILTERS(g)       getFilters(_activeFilters, (g))
#define GET_SUPPORTED_FILTERS(g)    getFilters(_supportedFilters, (g))

#define SET_REQUIRED_FILTERS(g, v)  setFilters(_requiredFilters, (g), (v))
#define SET_ACTIVE_FILTERS(g, v)    setFilters(_activeFilters, (g), (v))

//---------------------------------------------------------------------------
// Initialize an IOFWInterface instance. Instance variables are
// initialized, and an arpcom structure is allocated.
bool IOFWInterface::init(IONetworkController *controller)
{
    OSData * macAddr;

    // IONetworkInterface will call getIfnet() in its init() method,
    // so arpcom must be available before calling super::init().
    // First, fetch the controller's MAC/FireWire address.

    macAddr = OSDynamicCast(OSData, controller->getProperty(kIOMACAddress));
    if ( (macAddr == 0) || (macAddr->getLength() != FIREWIRE_ADDR_LEN) )
    {
        DLOG("%s: kIOMACAddress property access error (len %d)\n",
             getName(), macAddr ? macAddr->getLength() : 0);
        return false;
    }

    // Fetch an ifnet from DLIL. Depending on the 'uniqueid' provided,
    // an existing ifnet may be returned, or a new ifnet may be allocated.
    // For FireWire interface, the MAC address of the controller is used
    // as the unique ID. The size of the ifnet structure returned will be
    // large enough to hold the expanded arpcom structure.

    for ( int attempts = 0;
              attempts < 3 &&
                dlil_if_acquire(
                /* DLIL family  */ APPLE_IF_FAM_FIREWIRE,
                /* uniqueid     */ (void *) macAddr->getBytesNoCopy(),
                /* uniqueid_len */ macAddr->getLength(),
                /* ifp          */ (struct ifnet **) &_arpcom ) != 0;
              attempts++ )
	{
        // Perhaps the hardware was removed and then quickly re-inserted
        // into the system, and the stale interface object has not yet
        // released the ifnet back to DLIL. Since the new interface will
        // provide the same MAC address, DLIL will return an error and
        // refuse to allow multiple interfaces to share the same ifnet.
        // Wait a bit and hope the old driver stack terminates quickly.
        
        DLOG("dlil_if_acquire() failed, sleeping...\n");
        IOSleep( 50 );
    }

    if ( _arpcom == 0 )
    {
        DLOG("IOFWInterface: arpcom allocation failed\n");
        return false;
    }

    // Pass the init() call to our superclass.

    if ( super::init(controller) == false )
        return false;

    // Add an IONetworkData with room to hold an IOFWStats structure.
    // This class does not reference the data object created, and no harm
    // is done if the data object is released or replaced.

    IONetworkData * data = IONetworkData::withInternalBuffer(
                                              kIOFWStatsKey,
                                              sizeof(IOFWStats));
    if (data)
    {
        addNetworkData(data);
        data->release();
    }

    // Create and initialize the filter dictionaries.

    _requiredFilters = OSDictionary::withCapacity(4);
    _activeFilters   = OSDictionary::withCapacity(4);

    if ( (_requiredFilters == 0) || (_activeFilters == 0) )
        return false;

    _supportedFilters = OSDynamicCast(OSDictionary,
                        controller->getProperty(kIOPacketFilters));
    if ( _supportedFilters == 0 ) return false;
    _supportedFilters->retain();

    // Controller's Unicast (directed) and Broadcast filters should always
    // be enabled. Those bits should never be cleared.

    if ( !SET_REQUIRED_FILTERS( gIONetworkFilterGroup,
                                kIOPacketFilterUnicast |
                                kIOPacketFilterBroadcast )
      || !SET_REQUIRED_FILTERS( gIOEthernetWakeOnLANFilterGroup, 0 )
      || !SET_ACTIVE_FILTERS(   gIONetworkFilterGroup, 0 )
      || !SET_ACTIVE_FILTERS(   gIOEthernetWakeOnLANFilterGroup, 0 ) )
    {
         return false;
    }

    // Publish filter dictionaries to property table.

    setProperty( kIORequiredPacketFilters, _requiredFilters );
    setProperty( kIOActivePacketFilters,   _activeFilters );

    return true;
}

//---------------------------------------------------------------------------
// Initialize the given ifnet structure. The argument specified is a pointer
// to an ifnet structure obtained through getIfnet(). IOFWInterface
// will initialize this structure in a manner that is appropriate for most
// FireWire interfaces, then call super::initIfnet() to allow the superclass
// to perform generic interface initialization.
//
// ifp: Pointer to the ifnet structure to be initialized.
//
// Returns true on success, false otherwise.

bool IOFWInterface::initIfnet(struct ifnet * ifp)
{
    super::initIfnet( ifp );

    // Set defaults suitable for FireWire interfaces.

    ifp->if_baudrate = 10000000;
    ifp->if_family   = APPLE_IF_FAM_FIREWIRE;

    setInterfaceType( IFT_IEEE1394 ); 
//  setMaxTransferUnit( FIREWIREMTU );
    setMediaAddressLength( FIREWIRE_ADDR_LEN );
    setMediaHeaderLength( FIREWIRE_HDR_LEN );
    setFlags( IFF_BROADCAST | IFF_SIMPLEX | IFF_NOTRAILERS,
              IFF_RUNNING   | IFF_MULTICAST );

    return true;
}

//---------------------------------------------------------------------------
// Free the IOFWInterface instance. The memory allocated
// for the arpcom structure is released.

void IOFWInterface::free()
{
    if ( _arpcom )
    {
        DLOG("%s%d: release ifnet %p\n", getNamePrefix(), getUnitNumber(), _arpcom);
        dlil_if_release( (struct ifnet *) _arpcom );
        _arpcom = 0;
    }

    if ( _requiredFilters )
    {
        _requiredFilters->release();
        _requiredFilters = 0;
    }

    if ( _activeFilters )
    {
        _activeFilters->release();
        _activeFilters = 0;
    }
    
    if ( _supportedFilters )
    {
        _supportedFilters->release();
        _supportedFilters = 0; 
    }

    super::free();
}

//---------------------------------------------------------------------------
// This method returns a pointer to an ifnet structure maintained
// by the family specific interface object. IOFWInterface
// allocates an arpcom structure in init(), and returns a pointer
// to that structure when this method is called.
//
// Returns a pointer to an ifnet structure.

struct ifnet * IOFWInterface::getIfnet() const
{
    return (&(_arpcom->ac_if));
}

//---------------------------------------------------------------------------
// The name of the interface advertised to the network layer
// is generated by concatenating the string returned by this method,
// and an unit number.
//
// Returns a pointer to a constant string "fw". Thus FireWire interfaces
// will be registered as fw0, fw1, etc.

const char * IOFWInterface::getNamePrefix() const
{
    return kIOFWInterfaceNamePrefix;
}

//---------------------------------------------------------------------------
// Prepare the 'FireWire' controller after it has been opened. This is called
// by IONetworkInterface after a controller has accepted an open from this 
// interface. IOFWInterface uses this method to inspect the controller,
// and to cache certain controller properties, such as its hardware address.
// This method is called with the arbitration lock held.
//
// controller: The controller object that was opened.
//
// Returns true on success, false otherwise
// (which will cause the controller to be closed).

bool IOFWInterface::controllerDidOpen(IONetworkController * ctr)
{
    bool                 ret = false;
    OSData *             addrData;
    IOFWAddress *  addr;

    do {
        // Call the controllerDidOpen() in superclass first.

        if ( (ctr == 0) || (super::controllerDidOpen(ctr) == false) )
             break;

        // If the controller supports some form of multicast filtering,
        // then set the ifnet IFF_MULTICAST flag.

        if ( GET_SUPPORTED_FILTERS(gIONetworkFilterGroup) &
             (kIOPacketFilterMulticast | kIOPacketFilterMulticastAll) )
        {
            setFlags(IFF_MULTICAST);
        }

        // Advertise Wake on Magic Packet feature if supported.
        
        if ( GET_SUPPORTED_FILTERS( gIOEthernetWakeOnLANFilterGroup ) &
             kIOFWWakeOnMagicPacket )
        {
            IOPMrootDomain * root = getPMRootDomain();
            if ( root ) root->publishFeature( "WakeOnMagicPacket" );
        }

        // Get the controller's MAC/FireWire address.

        addrData = OSDynamicCast(OSData, ctr->getProperty(kIOMACAddress));
        if ((addrData == 0) || (addrData->getLength() != FIREWIRE_ADDR_LEN))
        {
            DLOG("%s: kIOMACAddress property access error (len %d)\n",
                 getName(), addrData ? addrData->getLength() : 0);
            break;
        }

        addr = (IOFWAddress *) addrData->getBytesNoCopy();

#if 1   // Print the address
        IOLog("%s: FireWire address %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
              ctr->getName(),
              addr->bytes[0],
              addr->bytes[1],
              addr->bytes[2],
              addr->bytes[3],
              addr->bytes[4],
              addr->bytes[5],
              addr->bytes[6],
              addr->bytes[7]
			  );
#endif

        // Copy the hardware address we obtained from the controller
        // to the arpcom structure.

        bcopy(addr, _arpcom->ac_enaddr, FIREWIRE_ADDR_LEN);
       
        ret = true;
    }
    while (0);

    return ret;
}

//---------------------------------------------------------------------------
// When a close from our last client is received, the interface will
// close the controller. But before the controller is closed, this method
// will be called by our superclass to perform any final cleanup. This
// method is called with the arbitration lock held.
//
// IOFWInterface will ensure that the controller is disabled.
//
// controller: The currently opened controller object.

void IOFWInterface::controllerWillClose(IONetworkController * ctr)
{
    super::controllerWillClose(ctr);
}

//---------------------------------------------------------------------------
// Handle ioctl commands originated from the network layer.
// Commands not handled by this method are passed to our superclass.
//
// Argument convention is:
//
//    arg0 - (struct ifnet *)
//    arg1 - (void *)
//
// The commands handled by IOFWInterface are:
//
//    SIOCSIFADDR
//    SIOCSIFFLAGS
//    SIOCADDMULTI
//    SIOCDELMULTI
//
// Returns an error code defined in errno.h (BSD).

SInt32 IOFWInterface::performCommand( IONetworkController * ctr,
                                            UInt32                cmd,
                                            void *                arg0,
                                            void *                arg1 )
{
    SInt32  ret;

    assert( arg0 == _arpcom );

    if ( ctr == 0 ) return EINVAL;

    switch ( cmd )
    {
        case SIOCSIFFLAGS:
        case SIOCADDMULTI:
        case SIOCDELMULTI:
        case SIOCSIFADDR:
        case SIOCSIFMTU:
        case SIOCSIFLLADDR:
            ret = (int) ctr->executeCommand(
                             this,            /* client */
                             (IONetworkController::Action)
                                &IOFWInterface::performGatedCommand,
                             this,            /* target */
                             ctr,             /* param0 */
                             (void *) cmd,    /* param1 */
                             arg0,            /* param2 */
                             arg1 );          /* param3 */
            break;

        default:
            // Unknown command, let our superclass deal with it.
            ret = super::performCommand(ctr, cmd, arg0, arg1);
            break;
    }

    return ret;
}

//---------------------------------------------------------------------------
// Handle an ioctl command on the controller's workloop context.

int IOFWInterface::performGatedCommand(void * target,
                                             void * arg1_ctr,
                                             void * arg2_cmd,
                                             void * arg3_0,
                                             void * arg4_1)
{
    IOFWInterface		*self = (IOFWInterface *) target;
    IONetworkController *ctr  = (IONetworkController *) arg1_ctr;
    struct ifreq 		*ifr  = (struct ifreq*) arg4_1;
    SInt                 ret  = EOPNOTSUPP;
	
    // Refuse to perform controller I/O if the controller is in a
    // low-power state that makes it "unusable".

    if ( self->_controllerLostPower ||
        ( self->getInterfaceState() & kIONetworkInterfaceDisabledState ) )
         return EPWROFF;

    switch ( (UInt32) arg2_cmd )
    {
        case SIOCSIFADDR:
            ret = self->syncSIOCSIFADDR(ctr);
            break;

        case SIOCSIFFLAGS:
            ret = self->syncSIOCSIFFLAGS(ctr);
            break;

        case SIOCADDMULTI:
            ret = self->syncSIOCADDMULTI(ctr);
            break;

        case SIOCDELMULTI:
            ret = self->syncSIOCDELMULTI(ctr);
            break;

        case SIOCSIFMTU:
            ret = self->syncSIOCSIFMTU( ctr, ifr );
            break;

        case SIOCSIFLLADDR:
            ret = self->syncSIOCSIFLLADDR( ctr, ifr->ifr_addr.sa_data,
                                           ifr->ifr_addr.sa_len );
            break;
    }

    return ret;
}

//---------------------------------------------------------------------------
// enableController() is reponsible for calling the controller's enable()
// method and restoring the state of the controller. We assume that
// controllers can completely reset its state upon receiving a disable()
// method call. And when it is brought back up, the interface should
// assist in restoring the previous state of the FireWire controller.

IOReturn IOFWInterface::enableController(IONetworkController * ctr)
{
    IOReturn   ret     = kIOReturnSuccess;
    bool       enabled = false;

    assert(ctr);

    do {
        // Is controller already enabled? If so, exit and return success.

        if ( _ctrEnabled )
            break;

        // Send the controller an enable command.
   
        if ( (ret = ctr->doEnable(this)) != kIOReturnSuccess )
            break;     // unable to enable the controller.

        enabled = true;

        // Disable all Wake-On-LAN filters.

        disableFilter(ctr, gIOEthernetWakeOnLANFilterGroup, ~0,
                      kFilterOptionNoStateChange);

        // Restore current filter selection.

        SET_ACTIVE_FILTERS(gIONetworkFilterGroup, 0);
        ret = enableFilter(ctr, gIONetworkFilterGroup, 0,
                           kFilterOptionSyncPendingIO);
        if ( ret != kIOReturnSuccess ) break;

        // Restore multicast filter settings.

        syncSIOCADDMULTI(ctr);

        // Re-apply the user supplied link-level address.

        OSData * lladdr = OSDynamicCast(OSData, getProperty(kIOMACAddress));
        if ( lladdr && lladdr->getLength() == FIREWIRE_ADDR_LEN )
        {
            ctr->setHardwareAddress( lladdr->getBytesNoCopy(),
                                     lladdr->getLength() );
        }

        _ctrEnabled = true;

    } while (false);

    // Disable the controller if a serious error has occurred after the
    // controller has been enabled.

    if ( enabled && (ret != kIOReturnSuccess) )
    {
        ctr->doDisable(this);
    }

    return ret;
}

//---------------------------------------------------------------------------
// Handles SIOCSIFFLAGS ioctl command for FireWire interfaces. The network
// stack has changed the if_flags field in ifnet. Our job is to go
// through if_flags and see what has changed, and act accordingly.
//
// The fact that if_flags contains both generic and FireWire specific bits
// means that we cannot move some of the default flag processing to the
// superclass.

int IOFWInterface::syncSIOCSIFFLAGS(IONetworkController * ctr)
{
    UInt16    flags = getFlags();
    IOReturn  ret   = kIOReturnSuccess;

    if ( ( ((flags & IFF_UP) == 0) || _controllerLostPower ) &&
         ( flags & IFF_RUNNING ) )
    {
        // If interface is marked down and it is currently running,
        // then stop it.

        ctr->doDisable(this);
        flags &= ~IFF_RUNNING;
        _ctrEnabled = false;
    }
    else if ( ( flags & IFF_UP )                &&
              ( _controllerLostPower == false ) &&
              ((flags & IFF_RUNNING) == 0) )
    {
        // If interface is marked up and it is currently stopped,
        // then start it.

        if ( (ret = enableController(ctr)) == kIOReturnSuccess )
            flags |= IFF_RUNNING;
    }

    if ( flags & IFF_RUNNING )
    {
        IOReturn rc;

        // We don't expect multiple flags to be changed for a given
        // SIOCSIFFLAGS call.

        // Promiscuous mode

        rc = (flags & IFF_PROMISC) ?
             enableFilter(ctr, gIONetworkFilterGroup,
                          kIOPacketFilterPromiscuous) :
             disableFilter(ctr, gIONetworkFilterGroup,
                           kIOPacketFilterPromiscuous);

        if (ret == kIOReturnSuccess) ret = rc;

        // Multicast-All mode

        rc = (flags & IFF_ALLMULTI) ?
             enableFilter(ctr, gIONetworkFilterGroup,
                          kIOPacketFilterMulticastAll) :
             disableFilter(ctr, gIONetworkFilterGroup,
                           kIOPacketFilterMulticastAll);

        if (ret == kIOReturnSuccess) ret = rc;
    }

    // Update the flags field to pick up any modifications. Also update the
    // property table to reflect any flag changes.

    setFlags(flags, ~flags);

    return errnoFromReturn(ret);
}

//---------------------------------------------------------------------------
// Handles SIOCSIFADDR ioctl.

SInt IOFWInterface::syncSIOCSIFADDR(IONetworkController * ctr)
{
    struct ifnet *       ifp = getIfnet();
    IOReturn ret = kIOReturnSuccess;
    struct ifaddr 		*ifa;
    struct sockaddr_dl * sdl;
	char	lladdr[8];
	
	ifa = ifnet_addrs[ifp->if_index - 1];
	if (ifa == NULL)
		return (EINVAL);

	//
	// Set the sdl LLADDR to 64 bit address - 3208790
	//
    sdl = (struct sockaddr_dl *)ifa->ifa_addr;
    if (sdl == NULL)
        return (EINVAL);

	bcopy(LLADDR(sdl), lladdr, FIREWIRE_ADDR_LEN);

	sdl->sdl_alen = FIREWIRE_ADDR_LEN;
	
    bcopy(_arpcom->ac_enaddr, LLADDR(sdl), FIREWIRE_ADDR_LEN);
	// end 

    // Interface is implicitly brought up by an SIOCSIFADDR ioctl.
    setFlags(IFF_UP);

    if ( (getFlags() & IFF_RUNNING) == 0 )
    {
        if ( (ret = enableController(ctr)) == kIOReturnSuccess )
		{
            setFlags(IFF_RUNNING);

			/*
			* Also send gratuitous ARPs to notify other nodes about
			* the address change.
			*/
			TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link)
			{
				if (ifa->ifa_addr != NULL &&
					ifa->ifa_addr->sa_family == AF_INET)
				{
					firewire_arp_ifinit(_arpcom, ifa);
				}
			}
			
		}
    }
	
    return errnoFromReturn(ret);
}

//---------------------------------------------------------------------------
// Handle SIOCADDMULTI ioctl command.

SInt IOFWInterface::syncSIOCADDMULTI(IONetworkController * ctr)
{
    IOReturn ret;

    // Make sure multicast filter is active.

    ret = enableFilter(ctr, gIONetworkFilterGroup, kIOPacketFilterMulticast);

    if ( ret == kIOReturnSuccess )
    {
        // Load multicast addresses only if the filter was activated.

        ret = setupMulticastFilter(ctr);

        // If the list is now empty, then deactivate the multicast filter.

        if ( _mcAddrCount == 0 )
        {
            IOReturn dret = disableFilter(ctr, gIONetworkFilterGroup,
                                          kIOPacketFilterMulticast);

            if (ret == kIOReturnSuccess) ret = dret;
        }
    }

    return errnoFromReturn(ret);
}

//---------------------------------------------------------------------------
// Handle SIOCDELMULTI ioctl command.

SInt IOFWInterface::syncSIOCDELMULTI(IONetworkController * ctr)
{
    return syncSIOCADDMULTI(ctr);
}

//---------------------------------------------------------------------------
// Handle SIOCSIFMTU ioctl.

int IOFWInterface::syncSIOCSIFMTU( IONetworkController * ctr,
                                         struct ifreq *        ifr )
{
#define MTU_TO_FRAMESIZE(x) \
        ((x) + kIOFWCRCSize + sizeof(struct firewire_header))

    SInt32  error = 0;
    UInt32  size;
    UInt32  maxSize = kIOFWMaxPacketSize;  // 1518
    UInt32  ifrSize = MTU_TO_FRAMESIZE( ifr->ifr_mtu );
    UInt32  ctrSize = MTU_TO_FRAMESIZE( getMaxTransferUnit() );

    // If change is not necessary, return success without getting the
    // controller involved.

    if ( ctrSize == ifrSize )
        return 0;  // no change required

    if ( ctr->getMaxPacketSize( &size ) == kIOReturnSuccess )
        maxSize = max( size, kIOFWMaxPacketSize );

    if ( ifrSize > maxSize )
        return EINVAL;	// MTU is too large for the controller.

    // Message the controller if the new MTU requires a non standard
    // frame size, or if the controller is currently programmed to
    // support an extended frame size which is no longer required.

    if ( max( ifrSize, ctrSize ) > kIOFWMaxPacketSize )
    {
        IOReturn ret;
        ret = ctr->setMaxPacketSize( max(ifrSize, kIOFWMaxPacketSize) );
        error = errnoFromReturn( ret );
    }

	if ( error == 0 )
    {
        // Success, update the MTU in ifnet.
        setMaxTransferUnit( ifr->ifr_mtu );
    }

    return error;
}

//---------------------------------------------------------------------------

int IOFWInterface::syncSIOCSIFLLADDR( IONetworkController * ctr,
                                            const char * lladdr, int len )
{
    struct ifnet *       ifp = getIfnet();
    struct sockaddr_dl * sdl;
    struct ifaddr *      ifa;
    IOReturn             ret;

	IOLog("IOFWInterface: syncSIOCSIFLLADDR+\n");

    ifa = ifnet_addrs[ifp->if_index - 1];
    if (ifa == NULL)
        return (EINVAL);

	IOLog("IOFWInterface: syncSIOCSIFLLADDR %d\n", __LINE__);

    sdl = (struct sockaddr_dl *)ifa->ifa_addr;
    if (sdl == NULL)
        return (EINVAL);

	IOLog("IOFWInterface: syncSIOCSIFLLADDR %d\n", __LINE__);

    if (len != sdl->sdl_alen)   /* don't allow length to change */
        return (EINVAL);

	IOLog("IOFWInterface: syncSIOCSIFLLADDR %d\n", __LINE__);

    if (_ctrEnabled != true)    /* reject if interface is down */
        return (ENETDOWN);

	IOLog("IOFWInterface: syncSIOCSIFLLADDR %d\n", __LINE__);

    //
    // Inform the FireWire driver about the address change.
    //
    ret = ctr->setHardwareAddress( lladdr, len );

    if ( ret == kIOReturnSuccess )
    {
        bcopy(lladdr, _arpcom->ac_enaddr, len);
        bcopy(lladdr, LLADDR(sdl), len);
        setProperty(kIOMACAddress, (void *)lladdr, len);

        DLOG("%s: SIOCSIFLLADDR %02x:%02x:%02x:%02x:%02x:%02x:%02x::%02x\n",
              ctr->getName(),
              lladdr[0], lladdr[1], lladdr[2], lladdr[3]
              lladdr[4], lladdr[5], lladdr[6], lladdr[7]);

        /*
         * Also send gratuitous ARPs to notify other nodes about
         * the address change.
         */
        TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link)
		{
            if (ifa->ifa_addr != NULL &&
                ifa->ifa_addr->sa_family == AF_INET)
			{
				firewire_arp_ifinit(_arpcom, ifa);
			}
        }
    }
	else
	{
		IOLog("IOFWInterface: Implement this method to set ifa address\n");
	}

    return errnoFromReturn( ret );
}

//---------------------------------------------------------------------------
// Enable a packet filter.

#define getOneFilter(x)   ((x) & (~((x) - 1)))

IOReturn
IOFWInterface::enableFilter(IONetworkController * ctr,
                                  const OSSymbol *      group,
                                  UInt32                filters,
                                  IOOptionBits          options)
{
    IOReturn ret = kIOReturnSuccess;

    if ( options & kFilterOptionNotInsideGate )
    {
        options &= ~kFilterOptionNotInsideGate;

    	return ctr->executeCommand(
                           this,               /* client */
                           (IONetworkController::Action)
                               &IOFWInterface::enableFilter,
                           this,               /* target */
                           (void *) ctr,       /* param0 */
                           (void *) group,     /* param1 */
                           (void *) filters,   /* param2 */
                           (void *) options ); /* param3 */
    }

	if ( options & kFilterOptionDisableZeroBits )
    {
        ret = disableFilter(ctr, group, ~filters, options);
        if ( ret != kIOReturnSuccess) return ret;
    }

    // If the controller does not support the packet filter,
    // there's no need to proceed.

    if (( GET_SUPPORTED_FILTERS(group) & filters ) != filters)
        return kIOReturnUnsupported;

    do {
        // Add new filter to the set of required filters.

        UInt32 reqFilters = GET_REQUIRED_FILTERS(group) | filters;
        UInt32 actFilters = GET_ACTIVE_FILTERS(group);
        UInt32 resFilters = ( actFilters ^ reqFilters );
        
        if ( (options & kFilterOptionSyncPendingIO) == 0 )
        {
            // Restrict filter update by using 'filters' as a mask.
            resFilters &= filters;
        }

        while ( resFilters && ((options & kFilterOptionDeferIO) == 0) )
        {
            UInt32 oneFilter = getOneFilter(resFilters);

            // Send a command to the controller driver.

            ret = ctr->enablePacketFilter(group, oneFilter,
                                          actFilters, options);
            if ( ret != kIOReturnSuccess ) break;

            resFilters &= ~oneFilter;
            actFilters |= oneFilter;
        }

        if ( (options & kFilterOptionNoStateChange) == 0 )
            SET_REQUIRED_FILTERS(group, reqFilters);
        SET_ACTIVE_FILTERS(group, actFilters);
    }
    while (false);

    return ret;
}

//---------------------------------------------------------------------------
// Disable a packet filter.

IOReturn
IOFWInterface::disableFilter(IONetworkController * ctr,
                                   const OSSymbol *      group,
                                   UInt32                filters,
                                   IOOptionBits          options)
{
    IOReturn ret = kIOReturnSuccess;

#if 0
    if ( options & kFilterOptionNotInsideGate )
    {
        options &= ~kFilterOptionNotInsideGate;
    
    	return ctr->executeCommand(
                           this,               /* client */
                           (IONetworkController::Action)
                               &IOFWInterface::disableFilter,
                           this,               /* target */
                           (void *) ctr,       /* param0 */
                           (void *) group,     /* param1 */
                           (void *) filters,   /* param2 */
                           (void *) options ); /* param3 */
    }
#endif

    do {
        // Remove specified filter from the set of required filters.

        UInt32 reqFilters = GET_REQUIRED_FILTERS(group) & ~filters;
        UInt32 actFilters = GET_ACTIVE_FILTERS(group);
        UInt32 resFilters = ( actFilters ^ reqFilters ) & filters;

        while ( resFilters && ((options & kFilterOptionDeferIO) == 0) )
        {
            UInt32 oneFilter = getOneFilter(resFilters);

            // Send a command to the controller driver.

            ret = ctr->disablePacketFilter(group, oneFilter,
                                           actFilters, options);
            if ( ret != kIOReturnSuccess ) break;

            resFilters &= ~oneFilter;
            actFilters &= ~oneFilter;
        }

        if ( (options & kFilterOptionNoStateChange) == 0 )
            SET_REQUIRED_FILTERS(group, reqFilters);
        SET_ACTIVE_FILTERS(group, actFilters);
    }
    while (false);

    return ret;
}

//---------------------------------------------------------------------------
// Cache the list of multicast addresses and send a command to the
// controller to update the multicast list.

IOReturn
IOFWInterface::setupMulticastFilter(IONetworkController * ctr)
{
    void *               multiAddrs = 0;
    UInt                 mcount;
    OSData *             mcData = 0;
    struct ifnet *       ifp = (struct ifnet *) _arpcom;
    struct ifmultiaddr * ifma;
    IOReturn             ret = kIOReturnSuccess;
    bool                 ok;

    assert(ifp);

    // Update the multicast addresses count ivar.

    mcount = 0;
    for (ifma = ifp->if_multiaddrs.lh_first;
         ifma != NULL;
         ifma = ifma->ifma_link.le_next)
    {
        if ((ifma->ifma_addr->sa_family == AF_UNSPEC) ||
            (ifma->ifma_addr->sa_family == AF_LINK))
            mcount++;
    }
    _mcAddrCount = mcount;

    if ( mcount )
    {
        char * addrp;
            
        mcData = OSData::withCapacity(mcount * FIREWIRE_ADDR_LEN);
        if (!mcData)
        {
            DLOG("%s: no memory for multicast address list\n", getName());
            return kIOReturnNoMemory;
        }
        
        // Loop through the linked multicast structures and write the
        // address to the OSData.

        for (ifma = ifp->if_multiaddrs.lh_first;
             ifma != NULL;
             ifma = ifma->ifma_link.le_next)
        {
            if (ifma->ifma_addr->sa_family == AF_UNSPEC) 
                addrp = &ifma->ifma_addr->sa_data[0];
            else
                if (ifma->ifma_addr->sa_family == AF_LINK)
                addrp = LLADDR((struct sockaddr_dl *) ifma->ifma_addr);
                else
                continue;

            ok = mcData->appendBytes((const void *) addrp, FIREWIRE_ADDR_LEN);
            assert(ok);
        }

        multiAddrs = (void *) mcData->getBytesNoCopy();
        assert(multiAddrs);
    }

    // Issue a controller command to setup the multicast filter.

    ret = ((IOFWController *)ctr)->setMulticastList(
                                            (IOFWAddress *) multiAddrs,
                                            mcount);
    if (mcData)
    {
        if (ret == kIOReturnSuccess)
            setProperty(kIOMulticastAddressList, mcData);

        mcData->release();
    }
    else {
        removeProperty(kIOMulticastAddressList);
    }

    return ret;
}

//---------------------------------------------------------------------------
// Power management support.
//
// Handlers called, with the controller's gate closed, in response to a
// controller power state change.

IOReturn
IOFWInterface::controllerWillChangePowerState(
                               IONetworkController * ctr,
                               IOPMPowerFlags        flags,
                               UInt32                stateNumber,
                               IOService *           policyMaker )
{
	if ( ( (flags & IOPMDeviceUsable ) == 0) && 
         ( _controllerLostPower == false )   &&
         _ctrEnabled )
    {
        UInt32 filters;

        _controllerLostPower = true;

        // Get the "aggressiveness" factor from the policy maker.

        ctr->getAggressiveness( kPMEthernetWakeOnLANSettings, &filters );

        filters &= GET_SUPPORTED_FILTERS( gIOEthernetWakeOnLANFilterGroup );

        // Is the link up? If it is, leave the WOL filters intact,
        // otherwise mask out the WOL filters that would not function
        // without a proper link. This will reduce power consumption
        // for cases when a machine is put to sleep and there is no
        // network connection.

        OSNumber * linkStatusNumber = (OSNumber *) 
                                      ctr->getProperty( kIOLinkStatus );

        if ( ( linkStatusNumber == 0 ) ||
             ( ( linkStatusNumber->unsigned32BitValue() &
                 (kIONetworkLinkValid | kIONetworkLinkActive) ) ==
                  kIONetworkLinkValid ) )
        {
            filters &= ~( kIOFWWakeOnMagicPacket |
                          kIOFWWakeOnPacketAddressMatch );
        }

        // Before controller is disabled, program its wake up filters.
        // The WOL setting is formed by a bitwise OR between the WOL filter
        // settings, and the aggresiveness factor from the policy maker.

        enableFilter( ctr,
                      gIOEthernetWakeOnLANFilterGroup,
                      filters,
                      kFilterOptionNoStateChange |
                      kFilterOptionSyncPendingIO );

        // Call SIOCSIFFLAGS handler to disable the controller,
        // and mark the interface as Not Running.

        syncSIOCSIFFLAGS(ctr);
    }
    
    return super::controllerWillChangePowerState( ctr, flags,
                                                  stateNumber,
                                                  policyMaker );
}

IOReturn
IOFWInterface::controllerDidChangePowerState(
                               IONetworkController * ctr,
                               IOPMPowerFlags        flags,
                               UInt32                stateNumber,
                               IOService *           policyMaker )
{
    IOReturn ret = super::controllerDidChangePowerState( ctr, flags,
                                                         stateNumber,
                                                         policyMaker );

    if ( ( flags & IOPMDeviceUsable ) && ( _controllerLostPower == true ) )
    {
        _controllerLostPower = false;

        // Clear _controllerLostPower, then call the SIOCSIFFLAGS handler to
        // perhaps enable the controller, restore all FireWire controller
        // state, then mark the interface as Running.

        syncSIOCSIFFLAGS(ctr);
    }

    return ret;
}

#define kIONetworkInterfaceProperties   "IONetworkInterfaceProperties"

//---------------------------------------------------------------------------
// Handle a request to set properties from kernel or non-kernel clients.
// For non-kernel clients, the preferred mechanism is through an user
// client.

IOReturn IOFWInterface::setProperties( OSObject * properties )
{
    IOReturn       ret;
    OSDictionary * dict = (OSDictionary *) properties;
    OSNumber *     num;

    // Call IONetworkInterface::setProperties() first.

    ret = super::setProperties(properties);

    if ( (ret == kIOReturnSuccess) || (ret == kIOReturnUnsupported) )
    {
        dict = OSDynamicCast( OSDictionary,
                 dict->getObject(kIONetworkInterfaceProperties) );
        if ( dict )
        {
            dict = OSDynamicCast( OSDictionary,
                     dict->getObject(kIORequiredPacketFilters) );
            if ( dict )
            {
                num = OSDynamicCast( OSNumber,
                        dict->getObject(kIOFWWakeOnLANFilterGroup) );

                if ( num )
                {
                    ret = enableFilter( getController(),
                                        gIOEthernetWakeOnLANFilterGroup,
                                        num->unsigned32BitValue(),
                                        kFilterOptionDeferIO       |
                                        kFilterOptionNotInsideGate |
                                        kFilterOptionDisableZeroBits );
                }
            }
        }
    }

    return ret;
}

//---------------------------------------------------------------------------
// willTerminate

bool IOFWInterface::willTerminate( IOService *  provider,
                                         IOOptionBits options )
{
    bool ret = super::willTerminate( provider, options );

    // We assume that willTerminate() is always called from the
    // provider's work loop context.

    // Once the gated ioctl path has been blocked, disable the controller.
    // The hardware may have already been removed from the system.

    if ( _ctrEnabled && getController() )
    {
        DLOG("IOFWInterface::willTerminate disabling controller\n");
        getController()->doDisable( this );
        _ctrEnabled = false;
    }

    return ret;
}

//---------------------------------------------------------------------------

IOReturn IOFWInterface::attachToDataLinkLayer(	IOOptionBits options,
												void *       parameter )
{
	struct dlil_protomod_reg_str protomod_reg;
	IOReturn	ret = 0;

	firewire_family_init();

	ret = super::attachToDataLinkLayer( options, parameter );
    if (ret == kIOReturnSuccess)
    {
        bpfattach( getIfnet(), DLT_APPLE_IP_OVER_IEEE1394, sizeof(struct firewire_header) );

		if (ivedonethis)
		{	
			ivedonethis++;
//			IOLog("attachToDataLinkLayer %d", ivedonethis);
			return kIOReturnSuccess;
		}

		ivedonethis++;
					
		// IPv4 proto register
		bzero(&protomod_reg, sizeof(struct dlil_protomod_reg_str));
		protomod_reg.attach_proto = firewire_attach_inet;
		protomod_reg.detach_proto = firewire_detach_inet;
		ret = dlil_reg_proto_module(PF_INET, APPLE_IF_FAM_FIREWIRE, &protomod_reg);
		if(ret == EEXIST || ret == 0)
			ret = kIOReturnSuccess;
		else
			IOLog("ERROR: dlil_reg_proto_module for IPv4 over FIREWIRE %d", ret);

		// IPv6 proto register
		bzero(&protomod_reg, sizeof(struct dlil_protomod_reg_str));
		protomod_reg.attach_proto = firewire_attach_inet6;
		protomod_reg.detach_proto = firewire_detach_inet6;
		ret = dlil_reg_proto_module(PF_INET6, APPLE_IF_FAM_FIREWIRE, &protomod_reg);
		if(ret == EEXIST || ret == 0)
			ret = kIOReturnSuccess;
		else
			IOLog("ERROR: dlil_reg_proto_module for IPv6 over FIREWIRE %d", ret);
	}
	else
	{
		IOLog("ERROR: attachToDataLinkLayer %d", ret);
	}
	
    return ret;
}

void IOFWInterface::detachFromDataLinkLayer( IOOptionBits options,
                                                  void *       parameter )
{
	int ret = 0;
	int *detach_ret = (int*)parameter;
	
	if (ivedonethis == 1)
	{		
		ret = dlil_dereg_proto_module(PF_INET, APPLE_IF_FAM_FIREWIRE);
	
		if(ret == ENOENT)
			IOLog("no protocol module for IPv4 to deregister for APPLE_IF_FAM_FIREWIRE");
	
		ret = dlil_dereg_proto_module(PF_INET6, APPLE_IF_FAM_FIREWIRE);
	
		if(ret == ENOENT)
			IOLog("no protocol module for IPv6 to deregister for APPLE_IF_FAM_FIREWIRE");
	
		dlil_dereg_if_modules(APPLE_IF_FAM_FIREWIRE);
	}

	ivedonethis--;
//	IOLog("detachFromDataLinkLayer %d", ivedonethis);
	
//  super::detachFromDataLinkLayer( options, parameter );
	ret = dlil_if_detach(getIfnet());
	
	if(detach_ret) 
		*detach_ret = ret;
		
}

void IOFWInterface::setIfnetMTU(UInt32 mtu)
{
    setMaxTransferUnit(mtu-FIREWIRE_HDR_LEN);
}

void IOFWInterface::setIfnetSoftc(void* parameter)
{
	struct ifnet *ifp = getIfnet();
	
	ifp->if_softc = parameter;
}

