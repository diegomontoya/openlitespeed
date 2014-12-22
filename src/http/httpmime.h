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
#ifndef HTTPMIME_H
#define HTTPMIME_H


#include <util/autostr.h>
#include <http/expiresctrl.h>

#include <stddef.h>

#define MAX_MIME_LEN 256

class HttpHandler;
class MimeMap;
class MIList;
class StringList;
class MIMEMap;
class MIMESuffix;
class MIMESuffixMap;
class MIMESettingList;
class XmlNodeList;
class HttpVHost;
class ConfigCtx;

class MIMESetting
{
    AutoStr2          * m_psMIME;
    ExpiresCtrl         m_expires;
    const HttpHandler * m_pHandler;

public:
    MIMESetting();
    MIMESetting( const MIMESetting & rhs );
    ~MIMESetting();
    const AutoStr2 *    getMIME() const     {   return m_psMIME;    }
    ExpiresCtrl *       getExpires()        {   return &m_expires;  }
    const ExpiresCtrl * getExpires() const  {   return &m_expires;  }
    void  setMIME( AutoStr2 * pMIME )       {   m_psMIME = pMIME;   }
    void  setHandler( const HttpHandler * pHdlr );
    const HttpHandler * getHandler() const  {   return m_pHandler;  }
    void  inherit( const MIMESetting * pParent, int updateOnly );
};

typedef void (*FnUpdate)(MIMESetting * pSetting, void * pValue );

class HttpMime
{
public:
    HttpMime();
    HttpMime( const HttpMime& rhs);
    ~HttpMime();
private:
    MIMEMap         * m_pMIMEMap;
    MIMESuffixMap   * m_pSuffixMap;
    MIMESetting     * m_pDefault;

    void operator=( const HttpMime& rhs ) {}

    int processOneLine( const char* pFilePath, char* pLine, int lineNo );
    int inheritSuffix( const HttpMime * pParent );


public:
    MIMESetting * initDefault( char * pMIME = NULL );
    int loadMime( const char* pPropertyPath );
    int addUpdateMIME( char * pType, char * pDesc, const char * &reason,
                    int update = 1 );

    const MIMESetting* getFileMime( const char *pPath ) const;
    const MIMESetting* getFileMime( const char *pPath, int len ) const;
    const MIMESetting* getFileMimeByType( const char *pType ) const;
    const MIMESetting* getDefault() const {   return m_pDefault;  }
    const MIMESetting* getMIMESetting( char * pMime ) const;
    int inherit( HttpMime * pParent, int handlerOnly = 1);

    MIMESetting* getDefault() {   return m_pDefault;  }
    int updateMIME( char *pMIME, FnUpdate fn, void *pValue, const HttpMime * pParent );
    int setCompressableByType( const char * pValue, const HttpMime * pParent,
                                    const char * pLogId );
    int setExpiresByType( const char * pValue, const HttpMime * pParent,
                                const char * pLogId );
    int addType( const HttpMime * pParent, const char * pValue, const char * pLogId );
    int addMimeHandler( const char * suffix, char * pMime,
                        const HttpHandler * pHandler,
                        const HttpMime * pParent, const char * pLogId );
    
    static void releaseMIMEList();
    char compressable( const char * pMIME ) const;
    static void setCompressable( MIMESetting * pSetting, void * pValue );
    static void setExpire( MIMESetting * pSetting, void * pValue );
    static void setHandler( MIMESetting * pSetting, void * pValue );
    static int  needCharset( const char * pMIME );
    static int  isValidMimeType( const char* pDescr );
    static int  shouldKeepAlive( const char * pMIME );
    static int configScriptHandler1( HttpVHost *pVHost,
        const XmlNodeList *pList, HttpMime *pHttpMime);
    static int configScriptHandler2( HttpVHost *pVHost,
        const XmlNodeList *pList, HttpMime *pHttpMime);
    static void addMimeHandler(const HttpHandler *pHdlr, char *pMime, HttpMime *pHttpMime, 
                              const char *pSuffix );
};


#endif
