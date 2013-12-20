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
#include "httpmethod.h"
#include <inttypes.h>
#include <string.h>

http_method_t HttpMethod::parse2( const char * pMethod )
{
    register http_method_t method = 0;
    register char ch = *pMethod & ~0x20;
    switch( ch )
    {
    case 'G':
        method = HTTP_GET;
        break;
    case 'H':
        method = HTTP_HEAD;
        break;
    case 'O':
        method = HTTP_OPTIONS;
        break;
        
    case 'P':
        switch( *(pMethod+1) & ~0x20 )
        {
        case 'U':
            method = HTTP_PUT;
            break;
        case 'O':
            method = HTTP_POST;
            break;
        case 'R':
            if ( 'F' == (*(pMethod+4 ) & ~0x20 ) )
                method = DAV_PROPFIND;
            else
                method = DAV_PROPPATCH;
            break;
        }
        break;
    case 'D':
        method = HTTP_DELETE;
        break;
        
    case 'T':
        method = HTTP_TRACE;
        break;
        
    case 'C':
        switch( *(pMethod+2) & ~0x20 )
        {
        case 'P':
            method = DAV_COPY;
            break;
        case 'N':
            method = HTTP_CONNECT;
            break;
        case 'E':
            if (( *(pMethod+6) & ~0x20 ) == 'O' )
                method = DAV_CHECKOUT;
            else
                method = DAV_CHECKIN;
            break;
        }
        break;
    case 'M':
        switch( *(pMethod+2) & ~0x20 )
        {
        case 'V':
            method = HTTP_MOVE;
            break;
        case 'C':
            method = DAV_MKCOL;
            break;
        case 'W':
            method = DAV_MKWORKSPACE;
            break;
        case 'A':
            method = DAV_MKACTIVITY;
            break;
        case 'R':
            method = DAV_MERGE;
            break;
        }
        break;
        
    case 'L':
        if (( *(pMethod+2) & ~0x20 ) == 'C' )
            method = DAV_LOCK;
        else 
            method = DAV_LABEL;
        break;
        
    case 'R':
        method = DAV_REPORT;
        break;
        
    case 'S':
        method = DAV_SEARCH;
        break;
        
    case 'U':
        switch( *(pMethod+2) & ~0x20 )
        {
        case 'L':
            method = DAV_UNLOCK;
            break;
        case 'D':
            method = DAV_UPDATE;
            break;
        case 'C':
            method = DAV_UNCHECKOUT;
            break;
        }
        break;
        
    case 'V':
        method = DAV_VERSION_CONTROL;
        break;
        
    case 'B':
        if ( ( *(pMethod+2) & ~0x20 ) == 'N' )
            method = DAV_BIND;
        else
            method = DAV_BASELINE_CONTROL;
        break;
        
    default:
        return 0;
    }
    if (method && (memcmp( s_psMethod[method], pMethod, s_iMethodLen[method] ) == 0 ))
    {
        //Experiment: ONLY allow GET/POST/HEAD
        if(method != HTTP_GET && method != HTTP_POST && method != HTTP_HEAD)
            return 0;
        return method;
     }
    return 0;
}


http_method_t HttpMethod::parse( const char * pMethod )
{
    http_method_t method = parse2( pMethod );
    if ( method )
    {
        char ch= *(pMethod + getLen( method ) );
        if (( ch != ' ' )&&( ch != '\t' ))
            return 0;
    }
    return method;
}



