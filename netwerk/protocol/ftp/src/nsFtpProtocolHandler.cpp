/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#include "nsFtpProtocolHandler.h"
#include "nsFtpProtocolConnection.h"
#include "nsIUrl.h"
#include "nsCRT.h"
#include "nsIComponentManager.h"
#include "nsIServiceManager.h"

static NS_DEFINE_CID(kStandardUrlCID,            NS_STANDARDURL_CID);

////////////////////////////////////////////////////////////////////////////////

nsFtpProtocolHandler::nsFtpProtocolHandler() {
    NS_INIT_REFCNT();
}

nsFtpProtocolHandler::~nsFtpProtocolHandler() {
}

NS_IMPL_ISUPPORTS(nsFtpProtocolHandler, nsIProtocolHandler::GetIID());

////////////////////////////////////////////////////////////////////////////////
// nsIProtocolHandler methods:

NS_IMETHODIMP
nsFtpProtocolHandler::GetScheme(const char* *result) const
{
    *result = nsCRT::strdup("ftp");
    if (*result == nsnull)
        return NS_ERROR_OUT_OF_MEMORY;
    return NS_OK;
}

NS_IMETHODIMP
nsFtpProtocolHandler::GetDefaultPort(PRInt32 *result) const
{
    *result = 21; 
    return NS_OK;
}

NS_IMETHODIMP
nsFtpProtocolHandler::MakeAbsoluteUrl(const char* aSpec,
                                       nsIUrl* aBaseUrl,
                                       char* *result) const
{
    // XXX optimize this to not needlessly construct the URL

    nsresult rv;
    nsIUrl* url;
    rv = NewUrl(aSpec, &url, aBaseUrl);
    if (NS_FAILED(rv)) return rv;

    rv = url->ToNewCString(result);
    NS_RELEASE(url);
    return rv;
}

NS_IMETHODIMP
nsFtpProtocolHandler::NewUrl(const char* aSpec,
                              nsIUrl* *result,
                              nsIUrl* aBaseUrl) const
{
    nsresult rv;

    // Ftp URLs (currently) have no additional structure beyond that provided by standard
    // URLs, so there is no "outer" given to CreateInstance 

    nsIUrl* url;
    rv = nsComponentManager::CreateInstance(kStandardUrlCID, nsnull,
                                            nsIUrl::GetIID(),
                                            (void**)&url);
    if (NS_FAILED(rv)) return rv;

    rv = url->Init(aSpec, aBaseUrl);

    nsIUrl* realUrl = nsnull;
    
    rv = url->QueryInterface(nsIUrl::GetIID(), (void**)&realUrl);
    if (NS_FAILED(rv)) return rv;

    // XXX this is the default port for ftp. we need to strip out the actual
    // XXX requested port.
    realUrl->SetPort(21);

    *result = realUrl;
    NS_ADDREF(*result);

    return rv;
}

NS_IMETHODIMP
nsFtpProtocolHandler::NewConnection(nsIUrl* url,
                                    nsISupports* eventSink,
                                    nsIEventQueue* eventQueue,
                                    nsIProtocolConnection* *result)
{
    nsresult rv;
    nsFtpProtocolConnection* connection = new nsFtpProtocolConnection();
    if (connection == nsnull)
        return NS_ERROR_OUT_OF_MEMORY;
    rv = connection->Init(url, eventSink, eventQueue);
    if (NS_FAILED(rv)) {
        delete connection;
        return rv;
    }
    NS_ADDREF(connection);
    *result = connection;
    return NS_OK;
}

////////////////////////////////////////////////////////////////////////////////
