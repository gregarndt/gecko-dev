/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/net/DNSRequestChild.h"
#include "mozilla/net/NeckoChild.h"
#include "nsIDNSRecord.h"
#include "nsHostResolver.h"
#include "nsTArray.h"
#include "nsNetAddr.h"
#include "nsIThread.h"
#include "nsThreadUtils.h"

using namespace mozilla::ipc;

namespace mozilla {
namespace net {

//-----------------------------------------------------------------------------
// ChildDNSRecord:
// A simple class to provide nsIDNSRecord on the child
//-----------------------------------------------------------------------------

class ChildDNSRecord : public nsIDNSRecord
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIDNSRECORD

  ChildDNSRecord(const DNSRecord& reply, uint16_t flags);

private:
  virtual ~ChildDNSRecord();

  nsCString mCanonicalName;
  nsTArray<NetAddr> mAddresses;
  uint32_t mCurrent; // addr iterator
  uint32_t mLength;  // number of addrs
  uint16_t mFlags;
};

NS_IMPL_ISUPPORTS(ChildDNSRecord, nsIDNSRecord)

ChildDNSRecord::ChildDNSRecord(const DNSRecord& reply, uint16_t flags)
  : mCurrent(0)
  , mFlags(flags)
{
  mCanonicalName = reply.canonicalName();

  // A shame IPDL gives us no way to grab ownership of array: so copy it.
  const nsTArray<NetAddr>& addrs = reply.addrs();
  uint32_t i = 0;
  mLength = addrs.Length();
  for (; i < mLength; i++) {
    mAddresses.AppendElement(addrs[i]);
  }
}

ChildDNSRecord::~ChildDNSRecord()
{
}

//-----------------------------------------------------------------------------
// ChildDNSRecord::nsIDNSRecord
//-----------------------------------------------------------------------------

NS_IMETHODIMP
ChildDNSRecord::GetCanonicalName(nsACString &result)
{
  if (!(mFlags & nsHostResolver::RES_CANON_NAME)) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  result = mCanonicalName;
  return NS_OK;
}

NS_IMETHODIMP
ChildDNSRecord::GetNextAddr(uint16_t port, NetAddr *addr)
{
  if (mCurrent >= mLength) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  memcpy(addr, &mAddresses[mCurrent++], sizeof(NetAddr));

  // both Ipv4/6 use same bits for port, so safe to just use ipv4's field
  addr->inet.port = port;

  return NS_OK;
}

// shamelessly copied from nsDNSRecord
NS_IMETHODIMP
ChildDNSRecord::GetScriptableNextAddr(uint16_t port, nsINetAddr **result)
{
  NetAddr addr;
  nsresult rv = GetNextAddr(port, &addr);
  if (NS_FAILED(rv)) return rv;

  NS_ADDREF(*result = new nsNetAddr(&addr));

  return NS_OK;
}

// also copied from nsDNSRecord
NS_IMETHODIMP
ChildDNSRecord::GetNextAddrAsString(nsACString &result)
{
  NetAddr addr;
  nsresult rv = GetNextAddr(0, &addr);
  if (NS_FAILED(rv)) {
    return rv;
  }

  char buf[kIPv6CStrBufSize];
  if (NetAddrToString(&addr, buf, sizeof(buf))) {
    result.Assign(buf);
    return NS_OK;
  }
  NS_ERROR("NetAddrToString failed unexpectedly");
  return NS_ERROR_FAILURE; // conversion failed for some reason
}

NS_IMETHODIMP
ChildDNSRecord::HasMore(bool *result)
{
  *result = mCurrent < mLength;
  return NS_OK;
}

NS_IMETHODIMP
ChildDNSRecord::Rewind()
{
  mCurrent = 0;
  return NS_OK;
}

NS_IMETHODIMP
ChildDNSRecord::ReportUnusable(uint16_t aPort)
{
  // "We thank you for your feedback" == >/dev/null
  // TODO: we could send info back to parent.
  return NS_OK;
}

//-----------------------------------------------------------------------------
// DNSRequestChild
//-----------------------------------------------------------------------------

DNSRequestChild::DNSRequestChild(const nsCString& aHost,
                                 const uint32_t& aFlags,
                                 nsIDNSListener *aListener,
                                 nsIEventTarget *target)
  : mListener(aListener)
  , mTarget(target)
  , mResultStatus(NS_OK)
  , mHost(aHost)
  , mFlags(aFlags)
{
}

void
DNSRequestChild::StartRequest()
{
  // we can only do IPDL on the main thread
  if (!NS_IsMainThread()) {
    NS_DispatchToMainThread(
      NS_NewRunnableMethod(this, &DNSRequestChild::StartRequest));
    return;
  }

  // Send request to Parent process.
  gNeckoChild->SendPDNSRequestConstructor(this, mHost, mFlags);

  // IPDL holds a reference until IPDL channel gets destroyed
  AddIPDLReference();
}

void
DNSRequestChild::CallOnLookupComplete()
{
  MOZ_ASSERT(mListener);

  mListener->OnLookupComplete(this, mResultRecord, mResultStatus);
}

bool
DNSRequestChild::Recv__delete__(const DNSRequestResponse& reply)
{
  MOZ_ASSERT(mListener);

  switch (reply.type()) {
  case DNSRequestResponse::TDNSRecord: {
    mResultRecord = new ChildDNSRecord(reply.get_DNSRecord(), mFlags);
    break;
  }
  case DNSRequestResponse::Tnsresult: {
    mResultStatus = reply.get_nsresult();
    break;
  }
  default:
    NS_NOTREACHED("unknown type");
    return false;
  }

  MOZ_ASSERT(NS_IsMainThread());

  bool targetIsMain = false;
  if (!mTarget) {
    targetIsMain = true;
  } else {
    mTarget->IsOnCurrentThread(&targetIsMain);
  }

  if (targetIsMain) {
    CallOnLookupComplete();
  } else {
    nsCOMPtr<nsIRunnable> event =
      NS_NewRunnableMethod(this, &DNSRequestChild::CallOnLookupComplete);
    mTarget->Dispatch(event, NS_DISPATCH_NORMAL);
  }

  return true;
}

//-----------------------------------------------------------------------------
// DNSRequestChild::nsISupports
//-----------------------------------------------------------------------------

NS_IMPL_ISUPPORTS(DNSRequestChild,
                  nsICancelable)

//-----------------------------------------------------------------------------
// DNSRequestChild::nsICancelable
//-----------------------------------------------------------------------------

NS_IMETHODIMP
DNSRequestChild::Cancel(nsresult reason)
{
  // for now Cancel is a no-op
  return NS_OK;
}

//------------------------------------------------------------------------------
}} // mozilla::net
