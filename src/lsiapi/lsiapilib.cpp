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
#include <lsiapi/lsiapi.h>

#include <http/accesslog.h>
#include <http/httpsession.h>
#include <http/httprespheaders.h>
#include <http/httputil.h>
#include <http/httpvhost.h>
#include <http/httpglobals.h>
#include <http/requestvars.h>
#include <http/staticfilecachedata.h>
#include <log4cxx/logger.h>
#include <lsiapi/envmanager.h>
#include <lsiapi/internal.h>
#include <lsiapi/modulehandler.h>
#include <lsiapi/modulemanager.h>

#include <util/datetime.h>
#include <util/ghash.h>
#include <util/gpath.h>
#include <util/ni_fio.h>
#include <util/vmembuf.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>

#include "util/configctx.h"


static int is_release_cb_added( const lsi_module_t *pModule, int level )
{
    return ((MODULE_DATA_ID( pModule )[level] != -1) ? 1: 0);
}

// static int is_cb_added( const lsi_module_t *pModule, lsi_callback_pf cb, int index )
// {
//     LsiApiHooks * pLsiApiHooks = (LsiApiHooks *)LsiApiHooks::getGlobalApiHooks( index );
//     if (pLsiApiHooks->find(pModule, cb))
//         return 1;
//     else
//         return 0;
// }

static int lsiapi_add_release_data_hook( int index, const lsi_module_t *pModule, lsi_callback_pf cb )
{
    LsiApiHooks * pHooks;
    if (( index < 0 )||( index >= LSI_MODULE_DATA_COUNT ))
        return -1;
    pHooks = LsiApiHooks::getReleaseDataHooks( index );
    if ( pHooks->add( pModule, cb, 0 ) >= 0 )
        return 0;
    return -1;
}

int add_global_hook( int index, const lsi_module_t *pModule, lsi_callback_pf cb, short order, short flag )
{
    const int *priority = MODULE_PRIORITY(pModule);
    LsiApiHooks * pHooks;
    if (( index < 0 )||( index >= LSI_HKPT_TOTAL_COUNT ))
        return -1;
    pHooks = (LsiApiHooks *) LsiApiHooks::getGlobalApiHooks( index );
    if (priority[index] < LSI_MAX_HOOK_PRIORITY && priority[index] > -1 * LSI_MAX_HOOK_PRIORITY)
        order = priority[index];
    if ( D_ENABLED( DL_MORE ))
            LOG_D(( "[Module: %s] add_global_hook, index %d [%s], priority %hd, flag %hd", 
                MODULE_NAME( pModule ), index, LsiApiHooks::s_pHkptName[index], order, flag ));
    return pHooks->add( pModule, cb, order, flag );
}

static int lsiapi_add_session_hook( lsi_session_t *session, int index, const lsi_module_t *pModule, 
                             lsi_callback_pf cb, short order, short flag )
{
    const int *priority = MODULE_PRIORITY(pModule);
    LsiApiHooks * pHooks;
    switch( index )
    {
    case LSI_HKPT_L4_BEGINSESSION:
    case LSI_HKPT_L4_ENDSESSION:
    case LSI_HKPT_L4_RECVING:
    case LSI_HKPT_L4_SENDING:
        pHooks = ((NtwkIOLink *)(LsiSession *)session)->getModSessionHooks( index );
        break;
        
    case LSI_HKPT_HTTP_BEGIN:
    case LSI_HKPT_RECV_REQ_HEADER:
    case LSI_HKPT_URI_MAP:
    case LSI_HKPT_RECV_REQ_BODY:
    case LSI_HKPT_RCVD_REQ_BODY:
    case LSI_HKPT_RECV_RESP_HEADER:
    case LSI_HKPT_RECV_RESP_BODY:
    case LSI_HKPT_RCVD_RESP_BODY:
    case LSI_HKPT_HANDLER_RESTART:
    case LSI_HKPT_SEND_RESP_HEADER:
    case LSI_HKPT_SEND_RESP_BODY:
    case LSI_HKPT_HTTP_END:
        pHooks = ((HttpSession *)(LsiSession *)session)->getModSessionHooks( index );
        break;
        
    default:
        return -1;
    }
    
    if (priority[index] < LSI_MAX_HOOK_PRIORITY && priority[index] > -1 * LSI_MAX_HOOK_PRIORITY)
        order = priority[index];
    if ( D_ENABLED( DL_MORE ))
            LOG_D(( "[Module: %s] add_session_hook, index %d [%s], priority %hd, flag %hd", 
                MODULE_NAME( pModule ), index, LsiApiHooks::s_pHkptName[index], order, flag ));
    return pHooks->add( pModule, cb, order, flag );
}

static int lsiapi_remove_session_hook( lsi_session_t *session, int index, const lsi_module_t *pModule )
{
    LsiApiHooks * pHooks;
    switch( index )
    {
    case LSI_HKPT_L4_BEGINSESSION:
    case LSI_HKPT_L4_ENDSESSION:
    case LSI_HKPT_L4_RECVING:
    case LSI_HKPT_L4_SENDING:
        pHooks = ((NtwkIOLink *)(LsiSession *)session)->getModSessionHooks( index );
        break;
        
    case LSI_HKPT_HTTP_BEGIN:
    case LSI_HKPT_RECV_REQ_HEADER:
    case LSI_HKPT_URI_MAP:
    case LSI_HKPT_RECV_REQ_BODY:
    case LSI_HKPT_RCVD_REQ_BODY:
    case LSI_HKPT_RECV_RESP_HEADER:
    case LSI_HKPT_RECV_RESP_BODY:
    case LSI_HKPT_HANDLER_RESTART:
    case LSI_HKPT_RCVD_RESP_BODY:
    case LSI_HKPT_SEND_RESP_HEADER:
    case LSI_HKPT_SEND_RESP_BODY:
    case LSI_HKPT_HTTP_END:
        pHooks = ((HttpSession *)(LsiSession *)session)->getModSessionHooks( index );
        break;
        
    default:
        return -1;
    }

    if ( D_ENABLED( DL_MORE ))
        LOG_D(( "[Module: %s] remove_session_hook, index %d", 
                MODULE_NAME( pModule ), index ));

    return pHooks->remove( pModule );
}

static  void log( lsi_session_t *session, int level, const char * fmt, ... )
{
    //For DEBUG level, may prevent to write to error log
    if ( ! D_ENABLED( DL_MORE ) && level == LSI_LOG_DEBUG)
        return ;
    
    char achFmt[4096];
    HttpSession * pSess = (HttpSession *)((LsiSession *)session);
    LOG4CXX_NS::Logger * pLogger = NULL;
    if ( pSess )
    {
        pLogger = pSess->getLogger();
        if ( pSess->getLogId() )
        {
            int n = snprintf( achFmt, sizeof( achFmt ) - 2, "[%s] %s", pSess->getLogId(), fmt );
            if ( (size_t)n > sizeof( achFmt ) - 2 )
                achFmt[sizeof(achFmt) - 2 ] = 0;
            fmt = achFmt;
        }
    }
    va_list ap;
    va_start( ap, fmt );
    HttpLog::vlog( pLogger, level, fmt, ap, 1 );
    va_end( ap );
        
}

static  void vlog( lsi_session_t *session, int level, const char * fmt, va_list vararg, int no_linefeed )
{
    char achFmt[4096];
    HttpSession * pSess = (HttpSession *)((LsiSession *)session);
    LOG4CXX_NS::Logger * pLogger = NULL;
    if ( pSess )
    {
        pLogger = pSess->getLogger();
        if ( pSess->getLogId() )
        {
            int n = snprintf( achFmt, sizeof( achFmt ) - 2, "[%s] %s", pSess->getLogId(), fmt );
            if ( (size_t)n > sizeof( achFmt ) - 2 )
                achFmt[sizeof(achFmt) - 2 ] = 0;
            fmt = achFmt;
        }
    }
    HttpLog::vlog( pLogger, level, fmt, vararg, no_linefeed );
    
}

static  void lograw( lsi_session_t *session, const char * buf, int len )
{
    HttpSession * pSess = (HttpSession *)((LsiSession *)session);
    LOG4CXX_NS::Logger * pLogger = NULL;
    if ( pSess )
        pLogger = pSess->getLogger();
    pLogger->lograw( buf, len );
}



static int lsiapi_register_env_handler(const char *env_name, unsigned int env_name_len, lsi_callback_pf cb)
{
    return EnvManager::getInstance().regEnvHandler(env_name, env_name_len, cb);
}

//If register a RELEASE hook, we need to make sure it is called only once, 
//and then upodate and record the module's _data_id
//Return -1 wrong level number
//Return -2, already inited
static int init_module_data( lsi_module_t *pModule, lsi_release_callback_pf cb, int level )
{
    if (level < LSI_MODULE_DATA_HTTP || level > LSI_MODULE_DATA_L4)
        return -1;
    
    if ( MODULE_DATA_ID( pModule )[level] == -1)
    {
        MODULE_DATA_ID( pModule )[level] = ModuleManager::getInstance().getModuleDataCount(level);
        ModuleManager::getInstance().incModuleDataCount(level);
        return lsiapi_add_release_data_hook(level, pModule, (lsi_callback_pf)cb);
    }
    
    return -2;
}


static LsiModuleData * get_module_data_by_type( void* obj, int type )
{
    NtwkIOLink * pNtwkLink;
    HttpSession *pSession;
    LsiSession * session = (LsiSession * )obj;
    switch( type )
    {
    case LSI_MODULE_DATA_HTTP:
    case LSI_MODULE_DATA_FILE:
    case LSI_MODULE_DATA_VHOST:
        pSession = dynamic_cast< HttpSession *>( session );
        if ( !pSession )
            return NULL;
        break;
    }
    
    switch( type )
    {
    case LSI_MODULE_DATA_L4:
        pNtwkLink = dynamic_cast< NtwkIOLink *>( session );
        if ( !pNtwkLink) //FIXME: make sure dealloc hookfunction is set. 
            break;
        return pNtwkLink->getModuleData();
    case LSI_MODULE_DATA_HTTP:
        return pSession->getModuleData();
    case LSI_MODULE_DATA_FILE:
        if ( !pSession->getSendFileInfo()->getFileData())
            break;
        return pSession->getSendFileInfo()->getFileData()->getModuleData();
    case LSI_MODULE_DATA_IP:
        pSession = dynamic_cast< HttpSession *>( session );
        if ( pSession )
            return pSession->getClientInfo()->getModuleData();
        else
        {
            pNtwkLink = dynamic_cast< NtwkIOLink *>( session );
            if ( pNtwkLink )
                return pNtwkLink->getClientInfo()->getModuleData();
        }
        break;
    case LSI_MODULE_DATA_VHOST:
        if ( !pSession->getReq()->getVHost() )
            break;
        return pSession->getReq()->getVHost()->getModuleData();
    }
    return NULL;
    
}

static int set_module_data ( lsi_session_t *session, const lsi_module_t *pModule, int level, void *data )
{
    if (level < LSI_MODULE_DATA_HTTP || level > LSI_MODULE_DATA_L4)
        return -1;
    
    if (!is_release_cb_added( pModule, level))
        return -1;
        
    int ret = -1;
    LsiModuleData * pData = get_module_data_by_type( session, level );
    
    if (pData)
    {
        if ( !pData->isDataInited())
        {
            if ( data )
                pData->initData(ModuleManager::getInstance().getModuleDataCount(level));
            else 
                return 0;
        }
        pData->set( MODULE_DATA_ID( pModule )[level], data);
        ret = 0;   
    }    
    
    return ret;   
}

static void * get_module_data( lsi_session_t *session, const lsi_module_t *pModule, int level )
{
    LsiModuleData * pData = get_module_data_by_type( session, level );
    if ( !pData )
        return NULL;
    return pData->get( MODULE_DATA_ID( pModule )[level] );
}

static void * get_cb_module_data( const lsi_cb_param_t * param, int level )
{
    LsiModuleData * pData = get_module_data_by_type( param->_session, level );
    if ( !pData )
        return NULL;
    
    return pData->get( MODULE_DATA_ID(((LsiApiHook *)param->_cur_hook)->_module)[level] );
}


static void free_module_data(lsi_session_t *session, const lsi_module_t *pModule, int level, lsi_release_callback_pf cb)
{
    LsiModuleData * pData = get_module_data_by_type( session, level );
    if ( !pData )
        return;
    void *data = pData->get( MODULE_DATA_ID( pModule )[level] );
    if (data)
    {
        cb(data);
        pData->set( MODULE_DATA_ID( pModule )[level], NULL );
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
typedef int  ( * appendDynBodyTermination_fun)  ( HttpSession *session, const char * pBuf, int len);
static int stream_write_next( lsi_cb_param_t * pParam, const char *buf, int len )
{
    if ( (((LsiApiHook *)pParam->_cur_hook) + 1) == pParam->_hook_info->_hooks->end() )
    {
        return ( *(appendDynBodyTermination_fun)(pParam->_hook_info->_termination_fp) )( 
                 (HttpSession *)((LsiSession *)pParam->_session), buf, len );
    }
    lsi_cb_param_t param;
    param._session = pParam->_session;
    param._cur_hook = (void *)(((LsiApiHook *)pParam->_cur_hook) + 1);
    param._hook_info = pParam->_hook_info;
    param._param = buf;
    param._param_len = len;
    param._flag_out = pParam->_flag_out;
    param._flag_in = pParam->_flag_in;
    return (*(((LsiApiHook *)param._cur_hook)->_cb))( &param );
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int lsiapi_stream_read_next( lsi_cb_param_t * pParam, char *pBuf, int size )
{
    if ( ((LsiApiHook *)pParam->_cur_hook) == pParam->_hook_info->_hooks->begin() )
    {
        return ( *(read_fp)(pParam->_hook_info->_termination_fp) )( 
                    (LsiSession *)pParam->_session, pBuf, size );
    }
    lsi_cb_param_t param;
    param._session = pParam->_session;
    param._cur_hook = (void *)(((LsiApiHook *)pParam->_cur_hook) - 1);
    param._hook_info = pParam->_hook_info;
    param._param = pBuf;
    param._param_len = size;
    param._flag_out = pParam->_flag_out;
    param._flag_in = pParam->_flag_in;
    return (*(((LsiApiHook *)param._cur_hook)->_cb))( &param );
    
}

static int lsiapi_stream_writev_next( lsi_cb_param_t * pParam, struct iovec *iov, int count )
{
    if ( (((LsiApiHook *)pParam->_cur_hook) + 1) == pParam->_hook_info->_hooks->end() )
    {
        return ( *(writev_fp)(pParam->_hook_info->_termination_fp) )( 
                 (LsiSession *)pParam->_session, iov, count );
    }
    lsi_cb_param_t param;
    param._session = pParam->_session;
    param._cur_hook = (void *)(((LsiApiHook *)pParam->_cur_hook) + 1);
    param._hook_info = pParam->_hook_info;
    param._param = iov;
    param._param_len = count;
    param._flag_out = pParam->_flag_out;
    param._flag_in = pParam->_flag_in;
    return (*((((LsiApiHook *)param._cur_hook))->_cb))( &param );
    
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int get_uri_file_path( lsi_session_t *session, const char *uri, int uri_len, char *path, int max_len )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpReq* pReq = pSession->getReq();
    
    if (uri[0] != '/')
        return -1;
    
    if (!pReq->getVHost())
        return -2;
    
    const AutoStr2 *rootStr = NULL;
    if (pReq->getContext())
        rootStr = pReq->getContext()->getRoot();
    else
        rootStr = pReq->getDocRoot();
    
    if (max_len <= rootStr->len() + uri_len)
        return -2; //Not enough space
    
    memcpy(path, rootStr->c_str(), rootStr->len());
    memcpy(path + rootStr->len(), uri, uri_len);
    path[rootStr->len() + uri_len] = 0x00;
    return 0;
}

static int set_resp_content_length( lsi_session_t *session, int64_t len  )
{
    HttpResp* pResp = ( (HttpSession *)((LsiSession *)session) )->getResp();
    pResp->setContentLen(len);
    pResp->appendContentLenHeader();
    return 0;
}

static int get_status_code( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpReq* pReq = pSession->getReq();
    return HttpStatusCode::indexToCode(pReq->getStatusCode());
}

static  void set_status_code( lsi_session_t *session, int code)
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return;
    HttpReq* pReq = pSession->getReq();
    int index = HttpStatusCode::codeToIndex(code);
    pReq->setStatusCode( index );
    pReq->updateNoRespBodyByStatus(index);
}

static int set_resp_header( lsi_session_t *session, unsigned int header_index, const char *name, int nameLen, const char *val, int valLen, int add_method )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpRespHeaders& respHeaders = pSession->getResp()->getRespHeaders();
    if (header_index < LSI_RESP_HEADER_END)
    {
        respHeaders.add((HttpRespHeaders::HEADERINDEX)header_index, val, valLen, add_method);
        if ( header_index == HttpRespHeaders::H_CONTENT_TYPE )
        {
            pSession->updateContentCompressible();
        }
    }
    else
        respHeaders.add(name, nameLen, val, valLen, add_method);
    return 0;
}

//multi headers supportted.
static int set_resp_header2( lsi_session_t *session, const char *s, int len, int add_method )
{
    int len0, len1;
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpRespHeaders& respHeaders = pSession->getResp()->getRespHeaders();
    
    //Save and check if the contenttype updated, if yes, processContentType
    //most case is change from NULL to a valid type.
    
    char *type0 = respHeaders.getContentTypeHeader(len0);
    respHeaders.parseAdd(s, len, add_method);
    char *type1 = respHeaders.getContentTypeHeader(len1);
    
    if (type0 != type1 || len0 != len1)
        pSession->updateContentCompressible();
    
    return 0;
}

static int get_resp_header(lsi_session_t *session, unsigned int header_index, const char *name, int nameLen, struct iovec *iov, int maxIovCount )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpRespHeaders& respHeaders = pSession->getResp()->getRespHeaders();
    if (header_index < LSI_RESP_HEADER_END)
        return respHeaders.getHeader((HttpRespHeaders::HEADERINDEX)header_index, iov, maxIovCount);
    else
        return respHeaders.getHeader(name, nameLen, iov, maxIovCount); 
}

//"status" and "version" will also be treated as header in SPDY if it already be put in
//and it will be counted.
static int get_resp_headers_count( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpRespHeaders& respHeaders = pSession->getResp()->getRespHeaders();
    return respHeaders.getHeadersCount(0);  //For API, retuen the non-spdy case
}

static int get_resp_headers( lsi_session_t *session, struct iovec *iov, int maxIovCount )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpRespHeaders& respHeaders = pSession->getResp()->getRespHeaders();
    return respHeaders.getAllHeaders( iov, maxIovCount );
}

static int remove_resp_header( lsi_session_t *session, unsigned int header_index, const char *name, int nameLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpRespHeaders& respHeaders = pSession->getResp()->getRespHeaders();
    if (header_index < LSI_RESP_HEADER_END)
        respHeaders.del((HttpRespHeaders::HEADERINDEX)header_index);
    else
        respHeaders.del(name, nameLen);
    return 0;
}


static int get_req_raw_headers_length( lsi_session_t *session)
{    
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpReq* pReq = pSession->getReq();
    return pReq->getHttpHeaderLen();
}

static int get_req_raw_headers( lsi_session_t *session, char *buf, int maxlen)
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpReq* pReq = pSession->getReq();
    const char *p = pReq->getHeaderBuf().begin();
    int size = pReq->getHttpHeaderLen();
    if (size > maxlen)
        size = maxlen;
    memcpy(buf, p, size);
    return size;
}

static int get_req_org_uri( lsi_session_t *session, char *buf, int buf_size )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpReq* pReq = pSession->getReq();
    
    int orgLen = pReq->getOrgReqURILen();
    if (buf_size > orgLen)
    {
        memcpy(buf, pReq->getOrgReqURL(), orgLen);
        buf[orgLen] = 0x00;
    }
    return orgLen;
}

static const char *get_req_uri( lsi_session_t *session, int * uri_len)
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return NULL;
    HttpReq* pReq = pSession->getReq();
    if ( uri_len )
        *uri_len = pReq->getURILen();
    return pReq->getURI();    
}

static const char* get_mapped_context_uri( lsi_session_t *session, int *length )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return NULL;
    HttpReq* pReq = pSession->getReq();
    if (pReq)
        *length = pReq->getContext()->getURILen();
    return pReq->getContext()->getURI();
}

static int register_req_handler( lsi_session_t *session, lsi_module_t *pModule, int scriptLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpReq* pReq = pSession->getReq();
    ModuleManager::iterator iter;
    iter = ModuleManager::getInstance().find( MODULE_NAME( pModule ) );
    if ( iter != ModuleManager::getInstance().end() &&
            ((LsiModule*)iter.second())->getModule()->_handler)
    {
        pReq->setHandler( (LsiModule*)iter.second() );
        pReq->setScriptNameLen(scriptLen);
        return 0;
    }
    
    return -1;
}

static int set_handler_write_state( lsi_session_t *session, int state )
{
    HttpSession* pSession = (HttpSession *)((LsiSession *)session);
    if (state)
    {
        pSession->continueWrite();
        pSession->setFlag( HSF_MODULE_WRITE_SUSPENDED|HSF_RESP_FLUSHED, 0 );
    }
    else
    {
        pSession->suspendWrite();
        pSession->setFlag( HSF_MODULE_WRITE_SUSPENDED, 1 );
    }
    return 0;
}

//return time_id, if error return -1
static int lsi_set_timer( unsigned int timeout_ms, lsi_timer_callback_pf timer_cb, void* timer_cb_param )
{
    static int timer_id = 0;
    ModuleTimer *pModuleTimer = new ModuleTimer;
    if (!pModuleTimer)
    {
        //LOG_ERROR (("lsi_set_timer new a time_t failed." ));
        return -1;
    }
    
    pModuleTimer->m_iId = timer_id++;
    pModuleTimer->m_tmExpire = DateTime::s_curTime + timeout_ms / 1000;
    pModuleTimer->m_tmExpireUs = DateTime::s_curTimeUs + 1000 * (timeout_ms % 1000);
    pModuleTimer->m_TimerCb = timer_cb;
    pModuleTimer->m_pTimerCbParam = timer_cb_param;
    
    LinkedObj *curPos = HttpGlobals::s_ModuleTimerList.head();
    ModuleTimer *pNext = NULL;
    while(curPos)
    {
        pNext = (ModuleTimer *)(curPos->next());
        if (pNext && ((pNext->m_tmExpire < pModuleTimer->m_tmExpire) ||
            (pNext->m_tmExpire == pModuleTimer->m_tmExpire && pNext->m_tmExpireUs <= pModuleTimer->m_tmExpireUs )))
            curPos = pNext;
        else
        {
            curPos->addNext(pModuleTimer);
            break;
        }
    }
    return pModuleTimer->m_iId;
}

static int lsi_remove_timer( int timer_id )
{
    LinkedObj *curPos = HttpGlobals::s_ModuleTimerList.head();
    ModuleTimer *pNext = NULL;
    while(curPos)
    {
        pNext = (ModuleTimer *)(curPos->next());
        if (pNext && pNext->m_iId == timer_id)
        {
            curPos->removeNext();
            delete pNext;
            break;
        }
        else
            curPos = pNext;
    }
    return 0;
}


static const char* get_req_header( lsi_session_t *session, const char *key, int keyLen, int *valLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return NULL;
    HttpReq* pReq = pSession->getReq();
    size_t idx = HttpHeader::getIndex( key );
    if ( idx != HttpHeader::H_HEADER_END )
    {
        *valLen = pReq->getHeaderLen( idx );
        return pReq->getHeader( idx );
    }
     
    return pReq->getHeader( key, keyLen, *valLen );
}

static const char* get_req_header_by_id( lsi_session_t *session, int idx, int *valLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return NULL;
    HttpReq* pReq = pSession->getReq();
    if (( idx >= 0 )&& (idx < HttpHeader::H_HEADER_END ))
    {
        *valLen = pReq->getHeaderLen( idx );
        return pReq->getHeader( idx );
    }
    return NULL;
}

static const char * get_req_cookies( lsi_session_t *session, int *len )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return NULL;
    return get_req_header( session, "Cookie", 6, len);
}

static const char *get_client_ip( lsi_session_t *session, int *len )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return NULL;
    *len = pSession->getPeerAddrStrLen();
    return pSession->getPeerAddrString();
}

static const char *get_req_query_string( lsi_session_t *session, int *len )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return NULL;
    HttpReq* pReq = pSession->getReq();
    if (len)
        *len = pReq->getQueryStringLen();
    return pReq->getQueryString();
}

static int get_req_cookie_count( lsi_session_t *session)
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    HttpReq* pReq = pSession->getReq();
    return RequestVars::getCookieCount( pReq );
}

static const char * get_cookie_value( lsi_session_t *session, const char * cookie_name, int nameLen, int *valLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL || !cookie_name || nameLen <= 0 )
        return NULL;
    HttpReq* pReq = pSession->getReq();
    return RequestVars::getCookieValue( pReq, cookie_name, nameLen, *valLen );
}

// static int get_req_body_file_fd( lsi_session_t *session )
// {
//     HttpSession *pSession = (HttpSession *)((LsiSession *)session);
//     if (pSession == NULL  )
//         return -1;
//     HttpReq* pReq = ( (HttpSession *)((LsiSession *)session) )->getReq();
//     if (!pReq->getBodyBuf())
//         return -1;
//     
//     if (pReq->getBodyBuf()->getfd() != -1)
//         return pReq->getBodyBuf()->getfd();
//     else
//     {
//         char file_path[50];
//         strcpy(file_path, "/tmp/lsiapi-XXXXXX");
//         int fd = mkstemp( file_path );
//         if (fd != -1)
//         {
//             unlink(file_path);
//             fcntl( fd, F_SETFD, FD_CLOEXEC );
//             long len;
//             pReq->getBodyBuf()->exactSize( &len );
//             if ( -1 == pReq->getBodyBuf()->copyToFile( 0, len, fd, 0 ))
//             {
//                 close(fd);
//                 return -1;
//             }
//         }
//         
//         return fd;
//     }
// }



static const char *lsi_req_env[LSI_REQ_COUNT] = {
    "REMOTE_ADDR",
    "REMOTE_PORT",
    "REMOTE_HOST",
    "REMOTE_USER",
    "REMOTE_IDENT",
    "REQUEST_METHOD",
    "QUERY_STRING",
    "AUTH_TYPE",
    "PATH_INFO",
    "SCRIPT_FILENAME",
    "REQUEST_FILENAME",
    "REQUEST_URI",
    "DOCUMENT_ROOT",
    "SERVER_ADMIN",
    "SERVER_NAME",
    "SERVER_ADDR",
    "SERVER_PORT",
    "SERVER_PROTOCOL",
    "SERVER_SOFTWARE",
    "API_VERSION",
    "THE_REQUEST",
    "IS_SUBREQ",
    "TIME",
    "TIME_YEAR",
    "TIME_MON",
    "TIME_DAY",
    "TIME_HOUR",
    "TIME_MIN",
    "TIME_SEC",
    "TIME_WDAY",
    "SCRIPT_NAME",
    "CURRENT_URI",
    "REQUEST_BASENAME",
    "SCRIPT_UID",
    "SCRIPT_GID",
    "SCRIPT_USERNAME",
    "SCRIPT_GROUPNAME",
    "SCRIPT_MODE",
    "SCRIPT_BASENAME",
    "SCRIPT_URI",
    "ORG_REQ_URI",
    "HTTPS",
    "SSL_VERSION",
    "SSL_SESSION_ID",
    "SSL_CIPHER",
    "SSL_CIPHER_USEKEYSIZE",
    "SSL_CIPHER_ALGKEYSIZE",
    "SSL_CLIENT_CERT",
    "GEOIP_ADDR",
    "PATH_TRANSLATED",
       
};

int getVarNameStr( const char *name, unsigned int len )
{
    int ret = -1;
    for (int i=0; i<LSI_REQ_COUNT; ++i)
    {
        if (strlen(lsi_req_env[i]) == len && strncasecmp(name, lsi_req_env[i], len) == 0)
        {
            ret = i;
            break;
        }
    }
    return ret;
}

static int get_req_var_by_id( lsi_session_t *session, int type, char *val, int maxValLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    int ret = -1;
    char *p = val;
    memset(val, 0, maxValLen);
    
    if (type <= LSI_REQ_VAR_HTTPS)
        ret = RequestVars::getReqVar( pSession, type - LSI_REQ_VAR_REMOTE_ADDR + REF_REMOTE_ADDR,  p, maxValLen );
    else if ( type < LSI_REQ_COUNT)
        ret = RequestVars::getReqVar2( pSession, type, p, maxValLen);
    else
        return -1;
    
    if ( p != val )
        memcpy( val, p, ret);
    
    if ( ret < maxValLen && ret >= 0 )
        val[ret] = 0;
    
    return ret;
}

static int  get_req_env( lsi_session_t *session, const char *name, unsigned int nameLen, char *val, int maxValLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return -1;
    const char *p;
    int valLen = 0;
    int type = getVarNameStr( name, nameLen );
    if (type != -1 )
    {
        return get_req_var_by_id( session, type, val, maxValLen );
    }
    else
    {
        p = RequestVars::getEnv( pSession, name, nameLen, valLen );
        if (!p || strlen(p) == 0)
        {
            p = getenv(name);
            if (p)
                valLen = strlen(p);
        }
        
        if (p)
        {
            if (valLen > maxValLen)
                valLen = maxValLen;
            memcpy(val, p, valLen);
        }
        return valLen;
    }
}

static void set_req_env( lsi_session_t *session, const char *name, unsigned int nameLen, const char *val, int valLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL )
        return;
//     assert( *(name + nameLen) == 0x00 );
//     assert( *(val + valLen) == 0x00 );    
    RequestVars::setEnv(pSession, name, nameLen, val, valLen);
}

static int get_req_content_length( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    HttpReq* pReq = pSession->getReq();
    return pReq->getContentLength();
}

static int read_req_body( lsi_session_t *session, char *buf, int bufLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    HttpReq* pReq = pSession->getReq();
    size_t size;
    char *p;
    if (pReq == NULL)
        return -1;
    
    if (pReq->getBodyBuf() == NULL)
        return -1;
    
    p = pReq->getBodyBuf()->getReadBuffer(size);
    
    //connection closed? in reading?
    if (size == 0)
    {
        if (pReq->getBodyRemain() > 0)
            return -1; //need wait and read again
    }
        
    if( (int)size > bufLen)
        size = bufLen;
    memcpy(buf, p, size);
    pReq->getBodyBuf()->readUsed(size);
    return size;
}

static int is_req_body_finished( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (!pSession || pSession->getFlag(HSF_REQ_BODY_DONE))
        return 1;
    else
        return 0;
}

static int set_req_wait_full_body( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    
    pSession->setFlag( HSF_REQ_WAIT_FULL_BODY );
    return 0;
}

static int set_resp_wait_full_body( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    
    pSession->setFlag( HSF_RESP_WAIT_FULL_BODY );
    return 0;
}

//is_resp_buffer_available() is to check if append_resp_body() and append_resp_bodyv() can be called ringt now
//through if append_resp_body() and append_resp_bodyv() is called, they won't failed
//but for better performance, user should call this function first to have a check
//if the response buffer to write is bigger than LSI_MAX_RESP_BUFFER_SIZE, 
//it will return 0 (false) means not available, otherwise return 1 (true)
//LSI_MAX_RESP_BUFFER_SIZE is defined in ls.h
static int is_resp_buffer_available(lsi_session_t *session)
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    
    if ( pSession->getRespCache() && pSession->getRespCache()->getCurWBlkPos() - pSession->getRespCache()->getCurRBlkPos() > LSI_MAX_RESP_BUFFER_SIZE)
        return 0;
    else
        return 1;
}

static int is_resp_buffer_gzippped(lsi_session_t *session)
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    
    if (pSession->getReq()->gzipAcceptable() & ( UPSTREAM_GZIP | UPSTREAM_DEFLATE ))
        return 1;
    else
        return 0;
}

//return 0 is OK, -1 error
static int append_resp_body( lsi_session_t *session, const char *buf, int len )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    
    if (pSession->isEndResponse())
        return -1;
    
    return pSession->appendDynBody( buf, len );
}


//return 0 is OK, -1 error
static int append_resp_bodyv( lsi_session_t *session, const struct iovec *vector, int count )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    
    if (pSession->isEndResponse())
        return -1;
    
    //FIXME: should use the below vector
    //return pSession->appendRespBodyBufV(vector, count);
    //FIXME: Currently, just use the appendDynBody for need to use the hook filters
    //THIS SHOULD BE UPDATED!!!
    bool error = 0;
    for ( int i=0; i<count; ++i)
    {
        if( pSession->appendDynBody((char *)((*vector).iov_base), (*vector).iov_len ) <= 0)
        {
            error = 1;
            break;
        }
        ++vector;
    }
    
    return error;
}

static int init_file_type_mdata( lsi_session_t *session, const lsi_module_t *pModule, const char *path, int pathLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    
    int ret = pSession->initSendFileInfo(path, strlen(path));
    if (ret)
        return -1;
    
    int fd = dup(pSession->getSendFileInfo()->getFileData()->getFileData()->getfd());
    return fd;
}
   
static int send_file( lsi_session_t *session, const char *path, int64_t start, int64_t size )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    
    if (pSession->isEndResponse())
        return -1;
    
    int ret = pSession->initSendFileInfo(path, strlen(path));
    if (ret)
        return ret;
    
    pSession->setSendFileOffsetSize(start, size);
    pSession->flush();
    return 0;
}

static void lsi_flush( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return;
    pSession->flush();
}

static void lsi_end_resp( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return;
    pSession->endResponse(1);
}

#define URI_OP_MASK     15
#define URL_QS_OP_MASK  112


static int set_uri_qs( lsi_session_t *session, int action, const char *uri, int uri_len, const char *qs, int qs_len )
{
#define MAX_URI_QS_LEN 8192
    char tmpBuf[MAX_URI_QS_LEN];
    char *pStart = tmpBuf;
    char *pQs;
    int final_qs_len = 0;
    int len = 0;
    int urlLen;
    
    int code;
    int uri_act;
    int qs_act;
    
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    if ( !action )
        return 0;
    uri_act = action & URI_OP_MASK;
    if (( uri_act == LSI_URL_REDIRECT_INTERNAL )&&
        ( pSession->getState() <= HSS_READING_BODY ))
        uri_act = LSI_URI_REWRITE;
    if (( uri_act == LSI_URI_NOCHANGE )||( !uri )||(uri_len == 0))
    {
        uri = pSession->getReq()->getURI();
        uri_len = pSession->getReq()->getURILen();
        action &= ~LSI_URL_ENCODED;
    }
    if ( (size_t)uri_len > sizeof( tmpBuf ) - 1 )
        uri_len = sizeof( tmpBuf ) - 1;

    switch( uri_act )
    {
    case LSI_URL_REDIRECT_INTERNAL:
    case LSI_URL_REDIRECT_301:
    case LSI_URL_REDIRECT_302:
    case LSI_URL_REDIRECT_303:
    case LSI_URL_REDIRECT_307: 
        if ( !(action & LSI_URL_ENCODED) )
            len = HttpUtil::escape( uri, uri_len, tmpBuf, sizeof( tmpBuf ) - 1 );
        else
        {
            memcpy( tmpBuf, uri, uri_len );
            len = uri_len;
        }
        break;
    default:
        if ( action & LSI_URL_ENCODED )
        {
            len = HttpUtil::unescape_n( uri, tmpBuf, uri_len );
        }
        else
        {
            memcpy( tmpBuf, uri, uri_len );
            len = uri_len;
        }
    }
    urlLen = len;
    
    pStart = &tmpBuf[len];
    qs_act = action & URL_QS_OP_MASK;
    if ( !qs || qs_len == 0 )
    {
        if (( qs_act == LSI_URL_QS_DELETE )||
            ( qs_act == LSI_URL_QS_SET ))
        {
            pQs = NULL;
            final_qs_len = 0;
        }
    }
    else 
    {
        *pStart++ = '?';
        pQs = pStart;
        if ( qs_act == LSI_URL_QS_DELETE )
        {
            //TODO: remove parameter from query string match the input
    //         {
    //             pQs = pSession->getReq()->getQueryString();
    //             final_qs_len = pSession->getReq()->getQueryString();
    //         }
            qs = NULL;
            qs_len = 0;
        }
        else if ( qs_act == LSI_URL_QS_APPEND )
        {
            final_qs_len = pSession->getReq()->getQueryStringLen();
            if ( final_qs_len > 0 )
            {
                memcpy(pStart, pSession->getReq()->getQueryString(), final_qs_len);
                pStart += final_qs_len++;
                *pStart++ = '&';
            }
        }
        else 
            qs_act = LSI_URL_QS_SET;
        if ( qs )
        {
            memcpy(pStart, qs, qs_len);
            final_qs_len += qs_len;
            pStart += qs_len;
        }
    }
    len = pStart - tmpBuf;
  
    switch( uri_act )
    {
    case LSI_URI_NOCHANGE:
    case LSI_URI_REWRITE:
        if ( uri_act != LSI_URI_NOCHANGE )
            pSession->getReq()->setRewriteURI( tmpBuf, urlLen, 1 );
        if ( qs_act & URL_QS_OP_MASK )
            pSession->getReq()->setRewriteQueryString( pQs, final_qs_len );
//        pSession->getReq()->addEnv(11);
        break;
    case LSI_URL_REDIRECT_INTERNAL:
        pSession->getReq()->setLocation(tmpBuf, len);
        pSession->setState( HSS_REDIRECT );
        pSession->continueWrite();
        break;
    case LSI_URL_REDIRECT_301:
    case LSI_URL_REDIRECT_302:
    case LSI_URL_REDIRECT_303:
    case LSI_URL_REDIRECT_307: 
        pSession->getReq()->setLocation( tmpBuf, len );
        
        if ( uri_act == LSI_URL_REDIRECT_307 )
            code = SC_307;
        else
            code = SC_301 + uri_act - LSI_URL_REDIRECT_301;
        pSession->getReq()->setStatusCode( code );
        pSession->setState( HSS_EXT_REDIRECT );
        pSession->continueWrite();
        break;
    default:
        break;
    }
    
    return 0;
}

static const char *  get_server_root()
{
    return HttpGlobals::s_pServerRoot;
}

static void *get_module_param( lsi_session_t *session, const lsi_module_t *pModule )
{
    ModuleConfig *pConfig = NULL;
    if (session)
        pConfig = ((LsiSession *)session)->getModuleConfig();
    else
        pConfig = ModuleManager::getGlobalModuleConfig();
        
    if (!pConfig)
        return NULL;
    
    return pConfig->get( MODULE_ID( pModule ) )->config;
    
}

static void * lsiapi_get_multiplexer()
{   return HttpGlobals::getMultiplexer();   }


static int get_file_path_by_uri( lsi_session_t *session, const char * uri, int uri_len, char * path, int max_len )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return -1;
    return pSession->getReq()->translatePath( uri, uri_len, path, max_len);
}

// static int set_static_file_by_uri( lsi_session_t *session, const char * uri, int uri_len )
// {
//     HttpSession *pSession = (HttpSession *)((LsiSession *)session);
//     if (pSession == NULL)
//         return -1;
//     //int HttpReq::translatePath( const char * pURI, int uriLen, char * pReal, int len ) const
//     //int ret = pSession->initSendFileInfo(path, strlen(path));
//     //if (ret)
//     //    return -1;
//     
// }
    
// static const struct stat * get_static_file_stat( lsi_session_t *session )
// {
//     HttpSession *pSession = (HttpSession *)((LsiSession *)session);
//     if (pSession == NULL)
//         return NULL;
//     SendFileInfo * pInfo = pSession->getSendFileInfo();
//     pInfo->getFileData();
// }
    
static const char * get_mime_type_by_suffix( lsi_session_t *session, const char * suffix )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (pSession == NULL)
        return NULL;
    return pSession->getReq()->getMimeBySuffix( suffix );
}
    
static int set_force_mime_type( lsi_session_t *session, const char * mime )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if ( pSession == NULL || !mime )
        return -1;
    pSession->getReq()->setForcedType( mime );
    return 0;
}

static const char * get_req_handler_type( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if ( pSession == NULL || pSession->getCurHandler() == NULL )
        return NULL;
    return HandlerType::getHandlerTypeString( pSession->getCurHandler()->getType() );
}

static int get_file_stat( lsi_session_t *session, const char *path, int pathLen, struct stat * st )
{
    if ( !path || !st )
        return -1;
    //TODO: use static file cache to serve file stats
    if ( session )
    {
        
    }
    return stat( path, st );
}


static const char * get_req_file_path( lsi_session_t *session, int * pathLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if ( pSession == NULL )
        return NULL;
    const AutoStr2 * pPath = pSession->getReq()->getRealPath();
    if ( !pPath )
    {
        if ( pathLen )
            *pathLen = 0;
        return NULL;
    }
    else
    {
        if ( pathLen )
            *pathLen = pPath->len();
        return pPath->c_str();
    }
}

static int  is_access_log_on( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if ( pSession == NULL )
        return -1;
    return !pSession->getFlag( HSF_ACCESS_LOG_OFF );
}
    
static void set_access_log( lsi_session_t *session, int enable )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if ( pSession == NULL )
        return;
    pSession->setFlag( HSF_ACCESS_LOG_OFF, !enable );

}

    
static int  get_access_log_string( lsi_session_t *session, const char * log_pattern, char * buf, int bufLen )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if (( pSession == NULL )||( !log_pattern )||( !buf ))
        return -1;
    
    CustomFormat * pLogFmt = AccessLog::parseLogFormat( log_pattern );
    return AccessLog::getLogString(pSession, pLogFmt, buf, bufLen );
}

static const char * get_module_name( const lsi_module_t * module )
{
    return MODULE_NAME( module ); 
}


static int is_resp_handler_aborted( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if ( pSession == NULL )
        return -1;
    //return pSession->isRespHandlerAborted();
    return 0;
}


static void * get_resp_body_buf( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if ( pSession == NULL )
        return NULL;
    return pSession->getRespCache();
}

static void * get_req_body_buf( lsi_session_t *session )
{
    HttpSession *pSession = (HttpSession *)((LsiSession *)session);
    if ( pSession == NULL )
        return NULL;
    return pSession->getReq()->getBodyBuf();
}
    
static int64_t get_body_buf_size( void * pBuf )
{
    if ( !pBuf )
        return -1;
    return ((VMemBuf *)pBuf)->getCurWOffset();
}

static int is_body_buf_eof( void * pBuf, int64_t offset )
{
    if ( !pBuf )
        return 1;
    return ((VMemBuf *)pBuf)->eof( offset );
}

static const char * acquire_body_buf_block( void * pBuf, int64_t offset, int * size )
{
    if ( !pBuf )
        return NULL;
    return ((VMemBuf *)pBuf)->acquireBlockBuf( offset, size );
    
}
    
static void  release_body_buf_block ( void * pBuf, int64_t offset )
{
    if ( !pBuf )
        return;
    ((VMemBuf *)pBuf)->releaseBlockBuf( offset );
}
    
static int   get_body_buf_fd( void * pBuf )
{
    if ( !pBuf )
        return -1;
    return ((VMemBuf *)pBuf)->getfd();
    
}


static void  reset_body_buf ( void * pBuf )
{
    if ( pBuf )
    {
        ((VMemBuf *)pBuf)->rewindReadBuf();
        ((VMemBuf *)pBuf)->rewindWriteBuf();
    }
}
    
static int   append_body_buf ( void * pBuf, const char * pBlock, int size )
{
    if ( !pBuf || !pBlock || size < 0 )
        return -1;
    return ((VMemBuf *)pBuf)->write( pBlock, size );

}



static const lsi_module_t * get_module( lsi_cb_param_t * param )
{
    return ((LsiApiHook *)param->_cur_hook)->_module;
}

static time_t get_cur_time( int32_t * usec )
{
    if ( usec )
        *usec = DateTime::s_curTimeUs;
    return DateTime::s_curTime;
}

int expand_current_server_varible( int level, const char *pVarible, char *buf, int maxLen )
{
    int ret = -1;
    switch ( level )
    {
    case LSI_SERVER_LEVEL:
    case LSI_LISTENER_LEVEL:
    case LSI_VHOST_LEVEL:
        ret = ConfigCtx::getCurConfigCtx()->expandVariable( pVarible, buf, maxLen, 1 );
        break;

    case LSI_CONTEXT_LEVEL:
    default:
        break;
    }
    
    return ret;
}

void lsiapi_init_server_api()
{
    lsi_api_t * pApi = LsiapiBridge::getLsiapiFunctions();
    pApi->add_session_hook = lsiapi_add_session_hook;
    pApi->remove_session_hook = lsiapi_remove_session_hook;
    pApi->register_env_handler = lsiapi_register_env_handler;
    pApi->get_module = get_module;

    
    pApi->get_server_root = get_server_root;
    pApi->get_module_param = get_module_param;
    pApi->init_module_data = init_module_data;

    pApi->get_module_data = get_module_data;   
    pApi->get_cb_module_data = get_cb_module_data;
    pApi->set_module_data = set_module_data;
    pApi->free_module_data = free_module_data;
    pApi->stream_writev_next = lsiapi_stream_writev_next;
    pApi->stream_read_next = lsiapi_stream_read_next;
        
    pApi->vlog = vlog;
    pApi->log = log;
    pApi->lograw = lograw;
    
    pApi->set_status_code = set_status_code;
    pApi->get_status_code = get_status_code; 
     
    pApi->register_req_handler = register_req_handler;
    pApi->set_handler_write_state = set_handler_write_state;
    pApi->set_timer = lsi_set_timer;
    pApi->remove_timer = lsi_remove_timer;
    pApi->get_req_raw_headers_length = get_req_raw_headers_length;
    pApi->get_req_raw_headers = get_req_raw_headers;
        
    pApi->get_req_header = get_req_header;
    pApi->get_req_header_by_id = get_req_header_by_id;
    pApi->get_req_org_uri = get_req_org_uri;
    pApi->get_req_uri = get_req_uri;
    pApi->get_uri_file_path = get_uri_file_path;
    pApi->get_mapped_context_uri = get_mapped_context_uri;
    
    pApi->get_client_ip = get_client_ip;
    pApi->get_req_query_string = get_req_query_string;
    pApi->get_req_cookies = get_req_cookies;
    pApi->get_req_cookie_count = get_req_cookie_count;
    pApi->get_cookie_value = get_cookie_value;
    
    
    pApi->get_req_env = get_req_env;
    pApi->set_req_env = set_req_env;
    pApi->get_req_var_by_id = get_req_var_by_id;
    pApi->get_req_content_length = get_req_content_length;
    pApi->read_req_body = read_req_body;
    pApi->is_req_body_finished = is_req_body_finished;
    pApi->is_resp_buffer_gzippped = is_resp_buffer_gzippped;
    pApi->set_req_wait_full_body = set_req_wait_full_body;
    pApi->set_resp_wait_full_body = set_resp_wait_full_body;
    
   
    pApi->is_resp_buffer_available = is_resp_buffer_available;
    pApi->append_resp_body = append_resp_body;
    pApi->append_resp_bodyv = append_resp_bodyv;
    pApi->send_file = send_file;
    pApi->init_file_type_mdata = init_file_type_mdata;
    pApi->flush = lsi_flush;
    pApi->end_resp = lsi_end_resp;
    
    pApi->set_resp_content_length = set_resp_content_length;
    pApi->set_uri_qs = set_uri_qs;
    
    //pApi->lsiapi_tcp_writev = lsiapi_tcp_writev;
    pApi->stream_write_next = stream_write_next;
    
   
    pApi->set_resp_header = set_resp_header;
    pApi->set_resp_header2 = set_resp_header2;
    pApi->get_resp_header = get_resp_header;
    pApi->get_resp_headers_count = get_resp_headers_count;
    pApi->get_resp_headers = get_resp_headers;
    pApi->remove_resp_header = remove_resp_header;
    pApi->get_multiplexer = lsiapi_get_multiplexer;

    pApi->get_file_path_by_uri = get_file_path_by_uri;
    //pApi->get_static_file_stat = get_static_file_stat;
    pApi->get_mime_type_by_suffix = get_mime_type_by_suffix;
    pApi->set_force_mime_type = set_force_mime_type;
    pApi->get_req_file_path = get_req_file_path;
    pApi->get_req_handler_type = get_req_handler_type;

    pApi->is_access_log_on = is_access_log_on;
    pApi->set_access_log = set_access_log;
    pApi->get_access_log_string = get_access_log_string;
    
    pApi->get_file_stat = get_file_stat;
    
    pApi->is_resp_handler_aborted = is_resp_handler_aborted;
    pApi->get_resp_body_buf = get_resp_body_buf;

    pApi->get_module_name = get_module_name;
    pApi->get_req_body_buf = get_req_body_buf;
    pApi->get_body_buf_size = get_body_buf_size;
    pApi->acquire_body_buf_block = acquire_body_buf_block;
    pApi->release_body_buf_block = release_body_buf_block;
    pApi->get_body_buf_fd = get_body_buf_fd;
    pApi->is_body_buf_eof = is_body_buf_eof;
    pApi->reset_body_buf = reset_body_buf;
    pApi->append_body_buf = append_body_buf;
    pApi->get_cur_time = get_cur_time;
    pApi->expand_current_server_varible = expand_current_server_varible;

    pApi->_debugLevel = HttpLog::getDebugLevel();
}
