/*
OPCClientToolKit
Copyright (C) 2005 Mark C. Beharrell

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA  02111-1307, USA.
*/

#ifndef OPCCLIENTTOOLKIT_H
#define OPCCLIENTTOOLKIT_H

#include "OPCClientToolKitDLL.h"  // DLL exports (__declspec(dllexport/dllimport))

// Core OPC COM headers (из вашего набора)
#include "opcda.h"      // OPC DA interfaces (IOPCServer, IOPCGroupStateMgt, etc.)
#include "opccomn.h"    // Common interfaces (IOPCShutdown, IOPCServerList)
#include "OpcEnum.h"    // Enumeration (OpcServerList CLSID)

// Toolkit wrappers (из вашего набора)
#include "OPCClient.h"  // COPCClient, exceptions, IAsynchDataCallback
#include "OPCHost.h"    // COPCHost, CLocalHost, CRemoteHost
#include "OPCServer.h"  // COPCServer, ServerStatus
#include "OPCGroup.h"   // COPCGroup, addItem, readSync/async, etc.
#include "OPCItem.h"    // COPCItem, read/write, properties
#include "OPCItemData.h" // OPCItemData, COPCItem_DataMap
#include "OPCProperties.h" // CPropertyDescription, SPropertyValue
#include "Transaction.h" // CTransaction, ITransactionComplete

// Additional includes if needed (from original)
#include <atlbase.h>
#include <atlstr.h>
#include <atlexcept.h>
#include <atlcoll.h>
#include <objbase.h>
#include <COMCat.h>

#endif // OPCCLIENTTOOLKIT_H