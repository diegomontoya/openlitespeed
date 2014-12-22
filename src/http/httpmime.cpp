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
#include "httpmime.h"
#include <http/httplog.h>
#include <http/httphandler.h>
#include <http/handlerfactory.h>
#include <http/httpglobals.h>
#include <util/gpointerlist.h>
#include <util/hashstringmap.h>
#include <util/stringlist.h>
#include <util/stringtool.h>
#include <util/xmlnode.h>
#include "util/configctx.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

static char DEFAULT_MIME_TYPE[] = "application/octet-stream";

static StringList * s_pMIMEList = NULL;

//class MIMESettingList : public TPointerList<MIMESetting>{};


static StringList* getMIMEList()
{
    if ( !s_pMIMEList )
    {
        s_pMIMEList = new StringList();
    }
    return s_pMIMEList;
}

void HttpMime::releaseMIMEList()
{
    if ( s_pMIMEList )
    {
        delete s_pMIMEList;
        s_pMIMEList = NULL;
    }
}


static AutoStr2 * getMIME( const char * pMIME )
{
    AutoStr2 * pStr = getMIMEList()->bfind( pMIME );
    if ( !pStr )
    {
        pStr = new AutoStr2( pMIME );
        if ( !pStr )
            return NULL;
        getMIMEList()->insert( pStr );
    }
    return pStr;
}

//static MIMESettingList * s_pOldSettings = NULL;
//static MIMESettingList* getOldSettings()
//{
//    if ( !s_pOldSettings )
//    {
//        s_pOldSettings = new MIMESettingList();
//    }
//    return s_pOldSettings;
//}

static const char SEPARATORS[] =
{ '(', ')', '<', '>', '@', ',', ';', ':', '\\', '"', '/', '[', ']', '?', '=',
  '{', '}', ' ', '\t'
};

static bool isValidToken(const char* pToken, int len)
{
    //FIXME: should use protocol definition.
    const char* pTemp = pToken;
    while ( *pTemp && len )
    {
        if ( (!isalnum(*pTemp)) && (*pTemp != '.') && (*pTemp != '+' )&&
             (*pTemp != '-') && (*pTemp != '_') )
            return false;
        pTemp ++;
        len --;
    }
    return true;

}

static bool isValidType( const char* pType )
{
    int len = strlen(pType) ;
    if ( len == 0 )
        return false;
    return isValidToken(pType, len);
}

int HttpMime::isValidMimeType( const char* pDescr )
{
    char* pTemp = (char *) strchr(pDescr, '/');
    if ( pTemp == NULL )
        return 0;
    int len = pTemp - pDescr ;
    if ( len == 0 || ! isValidToken( pDescr, len ))
        return 0;
    ++pTemp;
//    char* pTemp1;
//    pTemp1 = strchr( pTemp, ';' );
//
//    if ( pTemp1 )
//    {
//        while( isspace( *(pTemp1 - 1) ))
//            --pTemp1;
//        len = pTemp1 - pTemp;
//    }
//    else
        len = strlen(pTemp);
    if ( len <= 0 || ! isValidToken( pTemp, len ))
        return 0;
    return 1;
}

MIMESetting::MIMESetting()
    : m_psMIME( NULL )
    , m_pHandler( HandlerFactory::getInstance( 0, NULL ) )
{}

MIMESetting::MIMESetting( const MIMESetting & rhs )
    : m_psMIME( rhs.m_psMIME )
    , m_expires( rhs.m_expires )
    , m_pHandler( rhs.m_pHandler )
{
}


MIMESetting::~MIMESetting()
{
}

void MIMESetting::setHandler( const HttpHandler * pHdlr )
{
    m_pHandler = pHdlr;
}

void MIMESetting::inherit( const MIMESetting * pParent, int updateOnly )
{
    if ( !m_expires.cfgHandler() && (!m_pHandler->getHandlerType() || !updateOnly ) )
        m_pHandler = (HttpHandler *)pParent->getHandler();
    if ( !m_expires.cfgCompress() )
        m_expires.setCompressable( pParent->m_expires.compressable() );
    if ( !m_expires.cfgExpires() )
        m_expires.copyExpires( pParent->m_expires );
}



class MIMESubMap : public HashStringMap<MIMESetting*>
{
    AutoStr2        m_sMainType;
public:
    MIMESubMap()
        : HashStringMap<MIMESetting*>( 10 )
    {}
    MIMESubMap( const MIMESubMap& rhs );
    ~MIMESubMap()
    {   release_objects();  }
    
    MIMESetting* addMIME( char * pMIME );
    const char * setMainType( const char * pType, int len );
    const char * getMainType() const    {   return m_sMainType.c_str(); }
    void updateMIME( FnUpdate fpUpdate, void * pValue );
    int inherit( const MIMESubMap * pParent, int existedOnly );
    int inherit( iterator iter, int existedOnly );

};

MIMESubMap::MIMESubMap( const MIMESubMap& rhs )
    : HashStringMap<MIMESetting*>( 10 ), m_sMainType( rhs.m_sMainType )
{
    iterator iter;
    for( iter = rhs.begin(); iter != rhs.end(); iter = rhs.next( iter ) )
    {
        MIMESetting * pSetting = new MIMESetting( *iter.second() );
        if ( pSetting )
            insert( iter.first(), pSetting );
    }

}

int MIMESubMap::inherit( iterator iter, int existedOnly )
{
    iterator iter2 = find( iter.first() );
    if ( iter2 == end() )
    {
        if ( !existedOnly )
        {
            MIMESetting * pSetting = new MIMESetting( *iter.second() );
            if ( pSetting )
                insert( iter.first(), pSetting );
            else
                return -1;
        }
    }
    else
    {
        iter2.second()->inherit( iter.second(), existedOnly);
    }
    return 0;
}

int MIMESubMap::inherit( const MIMESubMap * pParent, int existedOnly )
{
    iterator iter;
    for( iter = pParent->begin();
         iter != pParent->end();
         iter = pParent->next( iter ) )
    {
        inherit( iter, existedOnly );
    }
    return 0;
}


const char * MIMESubMap::setMainType( const char * pType, int len )
{
    m_sMainType.setStr( pType, len);
    return m_sMainType.c_str();
}

void MIMESubMap::updateMIME( FnUpdate fpUpdate, void * pValue )
{
    iterator iter;
    for( iter = begin(); iter != end(); iter = next( iter ) )
    {
        (*fpUpdate)( iter.second(), pValue );
    }
}


MIMESetting* MIMESubMap::addMIME( char * pMIME )
{
    const char * p = strchr( pMIME, '/' );
    if ( !p )
        return NULL;
    ++p;
    if ( !*p )
        return NULL;
    AutoStr2 * pStr = ::getMIME( pMIME );
    if ( !pStr )
    {
        return NULL;
    }
    p = pStr->c_str() + ( p - pMIME );
    MIMESetting * pSetting = new MIMESetting();
    if ( !pSetting )
        return NULL;
    pSetting->setMIME( pStr );
    if ( strncasecmp( pStr->c_str(), "image/", 6 ) == 0 )
        pSetting->getExpires()->setBit( CONFIG_IMAGE );
    insert( p, pSetting );
    return pSetting;
}


class MIMEMap : public HashStringMap<MIMESubMap*>
{
public:
    MIMEMap()
        : HashStringMap<MIMESubMap*>( 10 )
    {}
    MIMEMap( const MIMEMap & rhs );
    ~MIMEMap()
    {   release_objects();  }
    iterator findSubMap( const char * pMIME, char * &p ) const;
    MIMESubMap * addSubMap( char * pMIME, int len );

    MIMESetting * addMIME( char * pMIME );
    MIMESetting * findMIME( char * pMIME ) const;
    void removeMIME( MIMESetting * pMIME );
    int updateMIME( char * pMIME, FnUpdate fpUpdate, void * pValue );
    int inherit( const MIMEMap * pParent, int existedOnly, char * pFilter );
    int inherit( iterator iter, int existedOnly );
};

MIMEMap::MIMEMap( const MIMEMap & rhs )
    : HashStringMap<MIMESubMap*>( 10 )
{
    iterator iter;
    for( iter = rhs.begin(); iter != rhs.end(); iter = rhs.next( iter ) )
    {
        MIMESubMap * pMap = new MIMESubMap( *iter.second() );
        if ( pMap )
            insert( pMap->getMainType(), pMap );
    }

}


int MIMEMap::inherit( iterator iter, int existedOnly )
{
    iterator iter2;
    iter2 = find( iter.first() );
    if ( iter2 == end() )
    {
        if ( !existedOnly )
        {
            MIMESubMap * pMap = new MIMESubMap( *iter.second() );
            if ( pMap )
                insert( pMap->getMainType(), pMap );
        }
    }
    else
    {
        iter2.second()->inherit( iter.second(), existedOnly );
    }
    return 0;
}

int MIMEMap::inherit( const MIMEMap * pParent, int existedOnly, char * pFilter )
{
    iterator iter;
    if (( pFilter == NULL )||( *pFilter == '*' ))
    {
        for( iter = pParent->begin();
             iter != pParent->end();
             iter = pParent->next( iter ) )
        {
            inherit( iter, existedOnly );
        }
    }
    else
    {

        char * p;
        iter = pParent->findSubMap( pFilter, p );
        if ( iter != pParent->end() )
        {
            MIMESubMap *pMap;
            iterator iter2;
            iter2 = find( iter.first() );
            if ( iter2 == end() )
            {
                if ( !existedOnly )
                {
                    pMap = addSubMap( pFilter, p - pFilter );
                }
                else
                    return 0;
            }
            else
                pMap = iter2.second();
            
            ++p;
            if ( *p == '*' )
            {
                pMap->inherit( iter.second(), existedOnly );
            }
            else
            {
                MIMESubMap::iterator iterSub= iter.second()->find( p );
                if ( iterSub == iter.second()->end() )
                    return 0;
                else
                {
                    pMap->inherit( iterSub, existedOnly );
                }
            }
        }

    }
    
    return 0;
}


MIMEMap::iterator MIMEMap::findSubMap( const char * pMIME, char * &p ) const
{
    p = (char *) strchr( pMIME, '/' );
    if ( !p )
        return end();
    *p = 0;
    iterator iter = find( pMIME );
    *p = '/';
    return iter;
}

MIMESubMap * MIMEMap::addSubMap( char * pMIME, int len )
{
    MIMESubMap *pMap = new MIMESubMap();
    if ( !pMap )
        return NULL;
    const char * pKey = pMap->setMainType( pMIME, len );
    insert( pKey, pMap );
    return pMap;
}

MIMESetting * MIMEMap::addMIME( char * pMIME )
{
    char * p;
    iterator iter = findSubMap( pMIME, p );
    MIMESubMap * pMap;
    if ( iter == end() )
    {
        pMap = new MIMESubMap();
        if ( !pMap )
            return NULL;
        const char * pKey = pMap->setMainType( pMIME, p - pMIME );
        insert( pKey, pMap );
    }
    else
        pMap = iter.second();
    MIMESubMap::iterator iterSub= pMap->find( p + 1 );

    if ( iterSub == pMap->end() )
    {
        return pMap->addMIME( pMIME );
    }
    else
        return iterSub.second();

}

MIMESetting * MIMEMap::findMIME( char * pMIME ) const
{
    char * p;
    iterator iter = findSubMap( pMIME, p );
    MIMESubMap * pMap; 
    if ( iter == end())
        return NULL;
    pMap = iter.second();
    ++p;
    char * p1 = strchr( p, ';' );
    char ch;
    if ( p1 )
    {
        while( isspace(*(p1 - 1)) )
            --p1;
        ch = *p1;
        *p1 = '\0';
    }
    MIMESubMap::iterator iterSub= pMap->find( p );
    if ( p1 )
        *p1 = ch;
    if ( iterSub == pMap->end() )
    {
        return NULL;
    }
    else
        return iterSub.second();

}

void MIMEMap::removeMIME( MIMESetting * pMIME )
{
    char * p;
    iterator iter = findSubMap( (char *)pMIME->getMIME()->c_str(), p );
    if ( iter == end() )
        return;
    iter.second()->remove( p + 1 );
}

int MIMEMap::updateMIME( char * pMIME, FnUpdate fnUpdate, void * pValue )
{
    if ( *pMIME == '*' )
    {
        iterator iter;
        for( iter = begin(); iter != end(); iter = next( iter ) )
        {
            iter.second()->updateMIME( fnUpdate, pValue );
        }
    }
    else
    {
        char * p;
        iterator iter = findSubMap( pMIME, p );

        if ( iter != end() )
        {
            MIMESubMap * pMap = iter.second();
            ++p;
            if ( *p == '*' )
                pMap->updateMIME( fnUpdate, pValue );
            else
            {
                MIMESubMap::iterator iterSub= pMap->find( p );
                if ( iterSub == pMap->end() )
                    return -1;
                else
                    (*fnUpdate)( iterSub.second(), pValue );
            }
        }
        else
            return -1;

    }
    return 0;
}

class MIMESuffix
{
public:
    MIMESuffix(const char* pSuffix, MIMESetting * pSetting);
    ~MIMESuffix() {}

    const char*     getSuffix() const   {   return m_sSuffix.c_str();   }
    MIMESetting*    getSetting() const  {   return m_pSetting;  }

    void            setSetting( MIMESetting * pSetting )
                    {   m_pSetting = pSetting;  }

private:
    AutoStr       m_sSuffix;
    MIMESetting * m_pSetting;

};

MIMESuffix::MIMESuffix(const char* pSuffix, MIMESetting * pSetting)
    : m_sSuffix( pSuffix )
    , m_pSetting( pSetting )
{}

class MIMESuffixMap : public HashStringMap<MIMESuffix*>
{
public:
    MIMESuffixMap()
        : HashStringMap<MIMESuffix*>( 10 )
        {}
    ~MIMESuffixMap() {   release_objects();   }
    MIMESuffixMap( int n ) : HashStringMap<MIMESuffix*>(n) {}
    MIMESuffix * addMapping( const char * pSuffix, MIMESetting * pSetting )
    {   return addUpdateMapping( pSuffix, pSetting, 0 );    }
    MIMESuffix * addUpdateMapping( const char * pSuffix, MIMESetting * pSetting,
                    int update = 1 );

};

MIMESuffix * MIMESuffixMap::addUpdateMapping(
        const char * pSuffix, MIMESetting * pSetting, int update )
{
    iterator iter = find( pSuffix );
    if ( iter != end() )
    {
        if ( update )
        {
            MIMESuffix * pOld = iter.second();
            GHash::erase( iter );
            if ( pOld )
                delete pOld;
        }
        else
            return NULL;
    }
    MIMESuffix * pSuffixMap = new MIMESuffix( pSuffix, pSetting );
    insert( pSuffixMap->getSuffix(), pSuffixMap );
    return pSuffixMap;
}


HttpMime::HttpMime()
    : m_pDefault( NULL )
{
    m_pMIMEMap = new MIMEMap();
    m_pSuffixMap = new MIMESuffixMap();

}

HttpMime::HttpMime( const HttpMime& rhs)
{
    m_pMIMEMap = new MIMEMap( *rhs.m_pMIMEMap );
    if ( rhs.m_pDefault )
        m_pDefault = m_pMIMEMap->findMIME( (char *)
                rhs.m_pDefault->getMIME()->c_str() );
    else
        m_pDefault = NULL;
    m_pSuffixMap = new MIMESuffixMap();
    inheritSuffix( &rhs );
}

int HttpMime::inheritSuffix( const HttpMime * pParent )
{
    MIMESuffixMap::iterator iter;
    for( iter = pParent->m_pSuffixMap->begin();
         iter != pParent->m_pSuffixMap->end();
         iter = pParent->m_pSuffixMap->next( iter ) )
    {
        MIMESetting * pSetting = m_pMIMEMap->findMIME( (char *)
                iter.second()->getSetting()->getMIME()->c_str() );
        if ( pSetting )
        {
            m_pSuffixMap->addUpdateMapping( iter.first(), pSetting, 0 );
        }
    }
    return 0;
}

int HttpMime::updateMIME( char *pMIME, FnUpdate fn, void * pValue,
                        const HttpMime * pParent )
{
    if ( pParent )
        m_pMIMEMap->inherit( pParent->m_pMIMEMap, 0, pMIME );
    return m_pMIMEMap->updateMIME( pMIME, fn, pValue );
}



MIMESetting * HttpMime::initDefault( char * pMIME )
{
    if ( !pMIME )
        pMIME = DEFAULT_MIME_TYPE;
    if ( !m_pDefault )
    {
        m_pDefault = m_pMIMEMap->addMIME( pMIME );
    }
    else
    {
        if ( !m_pDefault->getMIME()||
            (strcmp( pMIME, m_pDefault->getMIME()->c_str() ) != 0 ))
             m_pDefault->setMIME( ::getMIME( pMIME ));
    }
    return m_pDefault;
}

const MIMESetting* HttpMime::getMIMESetting( char * pMime ) const
{
    return m_pMIMEMap->findMIME( pMime );
}


HttpMime::~HttpMime()
{
    if ( m_pSuffixMap )
    {
        delete m_pSuffixMap;
        m_pSuffixMap = NULL;
    }
    if ( m_pMIMEMap )
    {
        delete m_pMIMEMap;
        m_pMIMEMap = NULL;
    }
}


const MIMESetting * HttpMime::getFileMime( const char *pPath, int len ) const
{
    const char * p = pPath + len;
    while( p > pPath )
    {
        if (*(p - 1) == '.')
            break;
        if (*(p - 1) == '/')
            return NULL;
        --p;
    }
    return getFileMimeByType( p );
}

const MIMESetting * HttpMime::getFileMime( const char *pPath ) const
{
    const char* pSuffix = strrchr( pPath, '.' );
    if ( pSuffix )
    {
        ++pSuffix;
        return getFileMimeByType( pSuffix );
    }
    return NULL;
}

const MIMESetting * HttpMime::getFileMimeByType( const char *pType ) const
{
    if ( pType )
    {
        MIMESuffixMap::iterator pos = m_pSuffixMap->find( pType );
        if ( pos != m_pSuffixMap->end() )
            return pos.second()->getSetting();
    }
    return NULL;
}

char HttpMime::compressable( const char * pMIME ) const
{
    const MIMESetting * pSetting = m_pMIMEMap->findMIME( (char *)pMIME );
    if ( !pSetting )
        if ( m_pDefault )
            return m_pDefault->getExpires()->compressable();
        else
            return 0;
    else
        return pSetting->getExpires()->compressable();
}

int HttpMime::inherit( HttpMime * pParent, int existedOnly )
{
    if ( !pParent )
        return 0;
    if ( m_pMIMEMap->inherit( pParent->m_pMIMEMap, existedOnly, NULL ) )
        return -1;
    inheritSuffix( pParent );
    if ( !existedOnly )
    {
        if ( pParent->m_pDefault )
            initDefault( (char *)pParent->getDefault()->getMIME()->c_str() );
    }
    return 0;
}


#define TEMP_BUF_LEN 1024

int HttpMime::loadMime( const char* pPropertyPath )
{
    FILE* fpMime = fopen(pPropertyPath, "r");
    if ( fpMime == NULL )
    {
        LOG_ERR(("[MIME] Cannot load property file: %s", pPropertyPath));
        return errno;
    }

    char pBuf[TEMP_BUF_LEN];
    int lineNo = 0;
    m_pSuffixMap->release_objects();

    while( ! feof(fpMime) )
    {
        lineNo ++ ;
        if ( fgets(pBuf, TEMP_BUF_LEN, fpMime) )
        {
            char * p = strchr( pBuf, '#' );
            if ( p )
                *p = 0;
            processOneLine( pPropertyPath, pBuf, lineNo);
        }
    }

    fclose(fpMime);
    return 0;
}

int HttpMime::addUpdateMIME( char * pType, char * pDesc, const char * &reason, int update )
{
    pType = StringTool::strtrim( pType );
    pDesc = StringTool::strtrim( pDesc );
    if ( !isValidMimeType(pDesc) )
    {
        reason = "invalid MIME description";
        return -1;
    }
    if ( strlen( pDesc ) > MAX_MIME_LEN )
    {
        reason = "MIME description is too long";
        return -1;
    }

    pType = StringTool::strlower( pType, pType );
    pDesc = StringTool::strlower( pDesc, pDesc );
    MIMESetting * pSetting = m_pMIMEMap->findMIME( pDesc );
    if ( pSetting )
    {
        if ( strcmp( pDesc, pSetting->getMIME()->c_str() ) != 0 )
        {
//            LOG_WARN(("[MIME] File %s line %d: MIME type with different parameter"
//                    " is not allowed, current: \"%s\" new: \"%s\", new one is used.",
//                            pFilePath, lineNo, pSetting->getMIME()->c_str(), pDesc));
            AutoStr2 * pMime = ::getMIME( pDesc );
            pSetting->setMIME( pMime );
        }
    }
    else
    {
        pSetting = m_pMIMEMap->addMIME( pDesc );
        if ( !pSetting )
        {
            reason = "invalid MIME type";
            return -1;
        }
    }
    char * p = pType;
    while( 1 )
    {
        p = strtok( pType, ", " );
        if ( !p )
            return 0;
        pType = NULL;
        p = StringTool::strtrim( p );
        while( *p == '.' )
            ++p;
        if ( *p == 0 )
        {
            continue;
        }
        if ( !isValidType(p) )
        {
            continue;
        }
        if ( strcmp( p, "default" ) == 0 )
        {
            m_pDefault = pSetting;
            continue;
        }
        if ( !m_pSuffixMap->addUpdateMapping( p, pSetting, update ) )
        {
            continue;
        }
    }
}

int HttpMime::processOneLine( const char* pFilePath, char* pLine, int lineNo )
{
    pLine = StringTool::strtrim( pLine );
    if ( strlen(pLine) == 0 )
        return 0;

    char* pType = pLine ;
    char* pDesc ;
    const char* reason;
    while(1)
    {
        if ( ( pDesc = strchr( pLine, '=' )) == NULL )
        {
            reason = "missing '='";
            break;
        }

        *pDesc = '\0';
        ++ pDesc;


        if ( addUpdateMIME( pType, pDesc, reason, 0 ) )
            break;
        else
            return 0;

    }

    LOG_ERR(("[MIME] File %s line %d: (%s) - \"%s\"",
                pFilePath, lineNo, reason, pLine));
    return -1;
}

void HttpMime::setCompressable( MIMESetting * pSetting, void * pValue )
{
    pSetting->getExpires()->setCompressable( (long)pValue );
    pSetting->getExpires()->setBit( CONFIG_COMPRESS );
}

int HttpMime::setCompressableByType( const char * pValue, const HttpMime * pParent,
                                    const char * pLogId )
{
    if ( !pValue )
        return 0;
    StringList list;
    list.split( pValue, strlen( pValue ) + pValue, "," );
    StringList::iterator iter;
    for( iter = list.begin(); iter != list.end(); ++iter )
    {
        char * pType = (*iter)->buf();
        long compressable = 1;
        if ( *pType == '!' )
        {
            compressable = 0;
            ++pType;
        }
        if ( updateMIME( pType, HttpMime::setCompressable,
                        (void *)compressable, pParent ) == -1)
        {
            LOG_NOTICE(( "[%s] Can not find compressible MIME type: %s, add it!",
                    pLogId, pType ));
            const char * pReason;
            char achTmp[]="";
            addUpdateMIME( achTmp, pType, pReason, 0 );
            updateMIME( pType, HttpMime::setCompressable,
                        (void *)compressable, pParent );
        }
    }
    return 0;
}


void HttpMime::setExpire( MIMESetting * pSetting, void * pValue )
{
    pSetting->getExpires()->parse( (const char *)pValue );
}

int HttpMime::setExpiresByType( const char * pValue, const HttpMime * pParent,
                                const char * pLogId )
{
    if ( !pValue )
        return 0;
    StringList list;
    list.split( pValue, strlen( pValue ) + pValue, "," );
    StringList::iterator iter;
    for( iter = list.begin(); iter != list.end(); ++iter )
    {
        char * pType = (*iter)->buf();
        char * pExpiresTime = strchr( pType, '=' );
        if ( !pExpiresTime )
        {
            pExpiresTime = strchr( pType, ' ' );
            if ( !pExpiresTime )
            {
                LOG_WARN(( "[%s] Invalid Expires setting: %s, ignore!", pLogId, pType ));
                continue;
            }
        }
        *pExpiresTime++ = 0;
        if ( updateMIME( pType, HttpMime::setExpire,
                pExpiresTime, pParent ) == -1)
        {
            LOG_WARN(( "[%s] Can not find MIME type: %s, "
                        "ignore Expires setting %s!",
                    pLogId, pType, pExpiresTime ));
        }
    }
    return 0;
}

int HttpMime::addType( const HttpMime * pDefault, const char * pValue, const char * pLogId )
{
    if ( !pValue )
        return 0;
    const char * reason;
//    StringList list;
//    list.split( pValue, strlen( pValue ) + pValue, "," );
//    StringList::iterator iter;
//    for( iter = list.begin(); iter != list.end(); ++iter )
//    {
    AutoStr str( pValue );
        char * pType = str.buf();
        char * pSuffix = strchr( pType, '=' );
        if ( !pSuffix )
        {
            pSuffix = strchr( pType, ' ' );
            if ( !pSuffix )
            {
                LOG_WARN(( "[%s] Incomplete MIME type, missing suffix: %s, ignore!", pLogId, pType ));
                return -1;
            }
        }
        *pSuffix++ = 0;
        m_pMIMEMap->inherit( pDefault->m_pMIMEMap, 0, pType );
        if ( addUpdateMIME( pSuffix, pType, reason, 1 ) )
            LOG_WARN(("[%s] failed to add mime type: %s %s, reason: %s",
                pLogId, pType, pSuffix, reason ));
//    }
    return 0;
}

void HttpMime::setHandler( MIMESetting * pSetting, void * pValue )
{
    pSetting->setHandler( (HttpHandler *)pValue );
}

int HttpMime::needCharset( const char * pMIME )
{
    if ( *pMIME != 't' )
        return 0;
    if (( strncmp( pMIME, "text/html", 9 ) != 0 )&&
        ( strncmp( pMIME, "text/plain", 10 ) != 0 ))
        return 0;
    return 1;
}

int HttpMime::shouldKeepAlive( const char * pMIME )
{
    if (strncmp( pMIME, "image/", 6 ) == 0 )
        return 1;
    if (strncmp( pMIME, "text/css", 8 ) == 0 )
        return 1;
    if (strncmp( pMIME, "application/x-javascript", 24 ) == 0 )
        return 1;
    return 0;
}

int HttpMime::addMimeHandler( const char * pSuffix, char * pMime, const HttpHandler * pHdlr,
                        const HttpMime * pParent, const char * pLogId )
{
    const char * reason;
    char achBuf[256];
    char achSuffix[512];
    if ( !pMime )
    {
        strcpy( achBuf, "application/x-httpd-" );

        pMime = (char *) strchr( pSuffix, ',' );
        if ( !pMime )
            pMime = (char *) strchr( pSuffix, ' ' );
        if ( pMime )
        {
            memccpy( &achBuf[20], pSuffix, 0, pMime - pSuffix );
            achBuf[20 + pMime - pSuffix ] = 0;
        }
        else
            strcpy( &achBuf[20], pSuffix );
        pMime = achBuf;
    }
    memccpy( achSuffix, pSuffix, 0, 511 );
    if ( addUpdateMIME( achSuffix, pMime, reason, 1 ) )
    {
        LOG_WARN(("[%s] failed to add mime type: %s %s, reason: %s",
            pLogId, pMime, pSuffix, reason ));
        return -1;
    }
    updateMIME( pMime, HttpMime::setHandler, (void *)pHdlr, pParent );
    return 0;
}
int HttpMime::configScriptHandler1( HttpVHost *pVHost,
        const XmlNodeList *pList, HttpMime *pHttpMime )
{
    ConfigCtx currentCtx( "scripthandler" );
    XmlNodeList::const_iterator iter;

    for( iter = pList->begin(); iter != pList->end(); ++iter )
    {
        XmlNode *pNode = *iter;
        char *pSuffix = ( char * ) pNode->getChildValue( "suffix" );

        if ( !pSuffix || !*pSuffix || strchr( pSuffix, '.' ) )
        {
            currentCtx.log_errorInvalTag( "suffix", pSuffix );
        }
        const HttpHandler *pHdlr = HandlerFactory::getHandler( pNode );
        addMimeHandler(pHdlr, ( char * ) pNode->getChildValue( "mime" ), pHttpMime, 
                              pSuffix );
    }
    return 0;
}
int HttpMime::configScriptHandler2( HttpVHost *pVHost,
        const XmlNodeList *pList, HttpMime *pHttpMime )
{
    ConfigCtx currentCtx( "scripthandler", "add" );
    XmlNodeList::const_iterator iter;

    for( iter = pList->begin(); iter != pList->end(); ++iter )
    {
        const char *value = ( char * )( *iter )->getValue();
        const char *pSuffixByTab = strchr( value, '\t' );
        const char *pSuffixBySpace = strchr( value, ' ' );
        const char *suffix  = NULL;

        if ( pSuffixByTab == NULL && pSuffixBySpace == NULL )
        {
            currentCtx.log_errorInvalTag( "ScriptHandler", value );
            continue;
        }
        else
            if ( pSuffixByTab == NULL )
                suffix = pSuffixBySpace;
            else
                if ( pSuffixBySpace == NULL )
                    suffix = pSuffixByTab;
                else
                    suffix = ( ( pSuffixByTab > pSuffixBySpace ) ? pSuffixBySpace : pSuffixByTab );

        const char *type = strchr( value, ':' );

        if ( suffix == NULL || type == NULL || strchr( suffix, '.' ) || type > suffix )
        {
            currentCtx.log_errorInvalTag( "suffix", suffix );
            continue;

        }

        ++ suffix; //should all spaces be handled here?? Now only one white space handled.

        char pType[256] = {0};
        memcpy( pType, value, type - value );

        char handler[256] = {0};
        memcpy( handler, type + 1, suffix - type - 2 );

        char achHandler[256] = {0};

        if ( currentCtx.expandVariable( handler, achHandler, 256 ) < 0 )
        {
            currentCtx.log_notice( "add String is too long for scripthandler, value: %s", handler );
            continue;
        }

        const HttpHandler *pHdlr = HandlerFactory::getHandler( pType, achHandler );
        addMimeHandler(pHdlr, NULL, pHttpMime, suffix );
    }

    return 0;
}
void HttpMime::addMimeHandler(const HttpHandler *pHdlr, char *pMime, HttpMime *pHttpMime, 
                              const char *pSuffix )
{
    if ( !pHdlr )
    {
        ConfigCtx::getCurConfigCtx()->log_error( "use static file handler for suffix [%s]", pSuffix );
        pHdlr = HandlerFactory::getInstance( 0, NULL );
    }
    HttpMime *pParent = NULL;

    if ( pHttpMime )
        pParent = HttpGlobals::getMime();
    else
        pHttpMime = HttpGlobals::getMime();

    pHttpMime->addMimeHandler( pSuffix, pMime,
                                pHdlr, pParent, LogIdTracker::getLogId() );
}


