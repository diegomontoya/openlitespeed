/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013  LiteSpeed Technologies, Inc.                        *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#include "handlerfactory.h"
#include "handlertype.h"
#include "httpextconnector.h"
#include "staticfilehandler.h"
#include <lsiapi/modulehandler.h>
#include "lsiapi/modulemanager.h"

#include <extensions/extworker.h>
#include <extensions/registry/extappregistry.h>

#include <http/httpvhost.h>
#include <http/httplog.h>
#include <util/objpool.h>
#include "util/configctx.h"
#include <util/xmlnode.h>

#include <stdio.h>

#include "ssi/ssiengine.h"
extern StaticFileHandler        s_staticFileHandler;
extern RedirectHandler          s_redirectHandler;
extern SSIEngine                s_ssiHandler;
extern ModuleHandler           s_ModuleHandler;

typedef ObjPool<HttpExtConnector>   ExtConnectorPool;
static ExtConnectorPool             s_extConnectorPool( 0, 10 );


ReqHandler* HandlerFactory::getHandler( int type )
{
    switch( type )
    {
    case HandlerType::HT_STATIC:
        return &s_staticFileHandler;
    case HandlerType::HT_MODULE:
        return &s_ModuleHandler;
    case HandlerType::HT_REDIRECT:
        return NULL;
    case HandlerType::HT_CGI:
    case HandlerType::HT_FASTCGI:
    case HandlerType::HT_SERVLET: //HT_JSP
    case HandlerType::HT_PROXY:
    case HandlerType::HT_LSAPI:
    case HandlerType::HT_LOADBALANCER:
        return s_extConnectorPool.get();
    default:
        return NULL;
    }
}


void HandlerFactory::recycle( HttpExtConnector * pHandler )
{
    s_extConnectorPool.recycle(  pHandler );
}

const HttpHandler * HandlerFactory::getInstance( int type, const char * pName )
{
    ModuleManager::iterator iter;
    
    switch( type )
    {
    case HandlerType::HT_STATIC:
        return &s_staticFileHandler;
    case HandlerType::HT_SSI:
        return &s_ssiHandler;        
    case HandlerType::HT_REDIRECT:
        return &s_redirectHandler;
    case HandlerType::HT_MODULE:
        iter = ModuleManager::getInstance().find(pName);
        if ( iter != ModuleManager::getInstance().end() &&
            ((LsiModule*)iter.second())->getModule()->_handler)
            return iter.second();
        else
            return NULL;
        
    case HandlerType::HT_CGI:
        pName = "lscgid";
    case HandlerType::HT_FASTCGI:
    case HandlerType::HT_PROXY:
    case HandlerType::HT_SERVLET:
    case HandlerType::HT_LSAPI:
    case HandlerType::HT_LOADBALANCER:
        if ( !pName )
            return NULL;
        return ExtAppRegistry::getApp( type - HandlerType::HT_CGI, pName );
    
    }
    return NULL;
}

const HttpHandler *HandlerFactory::getHandler(
        const char *pType, const char *pHandler )
{
    int role = HandlerType::ROLE_RESPONDER;
    int type = HandlerType::getHandlerType( pType, role );

    if ( ( type == HandlerType::HT_END ) ||
            ( type == HandlerType::HT_JAVAWEBAPP ) ||
            ( type == HandlerType::HT_RAILS ) )
    {
        ConfigCtx::getCurConfigCtx()->log_error( "Invalid External app type:[%s]" , pType );
        return NULL;
    }

    const HttpHandler *pHdlr = HandlerFactory::getInstance( type, pHandler );
    
    return pHdlr;
}

const HttpHandler *HandlerFactory::getHandler( const XmlNode *pNode )
{
    const char *pType = pNode->getChildValue( "type" );

    char achHandler[256];
    const char *pHandler = ConfigCtx::getCurConfigCtx()->getExpandedTag( pNode, "handler", achHandler, 256 );
    return getHandler( pType, pHandler );
}