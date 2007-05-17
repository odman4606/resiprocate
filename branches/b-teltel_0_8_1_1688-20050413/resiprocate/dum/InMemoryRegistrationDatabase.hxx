#if !defined(RESIP_INMEMORYREGISTRATIONDATABASE_HXX)
#define RESIP_INMEMORYREGISTRATIONDATABASE_HXX

#include <map>

#include "resiprocate/dum/RegistrationPersistenceManager.hxx"

namespace resip
{

/**
  Trivial implementation of a persistence manager. This class keeps
  all registrations in memory, and has no schemes for disk storage
  or replication of any kind. It's good for testing, but probably
  inappropriate for any commercially deployable products.
 */
class InMemoryRegistrationDatabase : public RegistrationPersistenceManager
{
  public:
    InMemoryRegistrationDatabase();
    virtual ~InMemoryRegistrationDatabase();

    virtual void addAor(Uri &aor, contact_list_t contacts = contact_list_t());
    virtual void removeAor(Uri &aor);
    virtual bool aorIsRegistered(Uri &aor);

    virtual void lockRecord(Uri &aor);
    virtual void unlockRecord(Uri &aor);

    virtual update_status_t updateContact(Uri &aor, Uri &contact, time_t expires);
    virtual void removeContact(Uri &aor, Uri &contact);

    virtual contact_list_t getContacts(Uri &aor);

  private:
    typedef std::map<Uri,contact_list_t> database_map_t;
    database_map_t mDatabase;
};
 
}

#endif

/* ====================================================================
 * The Vovida Software License, Version 1.0 
 * 
 * Copyright (c) 2000 Vovida Networks, Inc.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 
 * 3. The names "VOCAL", "Vovida Open Communication Application Library",
 *    and "Vovida Open Communication Application Library (VOCAL)" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact vocal@vovida.org.
 *
 * 4. Products derived from this software may not be called "VOCAL", nor
 *    may "VOCAL" appear in their name, without prior written
 *    permission of Vovida Networks, Inc.
 * 
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL VOVIDA
 * NETWORKS, INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 * IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * 
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by Vovida
 * Networks, Inc. and many individuals on behalf of Vovida Networks,
 * Inc.  For more information on Vovida Networks, Inc., please see
 * <http://www.vovida.org/>.
 *
 */