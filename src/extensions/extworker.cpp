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
#include "extworker.h"
#include "extconn.h"
#include "extrequest.h"
#include "extworkerconfig.h"
#include "pidlist.h"

#include <http/datetime.h>
#include <http/httplog.h>
#include <http/httpstatuscode.h>

#include <limits.h>

#include <signal.h>
#include <util/ssnprintf.h>

ExtWorker::ExtWorker()
    : m_pConfig( NULL )
    , m_pMultiplexer( NULL )
    , m_iRole( EXTAPP_RESPONDER )
    , m_iMultiplexConns( 0 )
    , m_iWantManagementInfo( 1 )
    , m_iErrors( 0 )
    , m_iState( 0 )
    , m_lErrorTime( 0 )
    , m_lLastRestart( 0 )
    , m_lIdleTime( 0 )
    , m_iLingerConns( 0 )
{
}

ExtWorker::~ExtWorker()
{
    if ( m_pConfig )
        delete m_pConfig;
}



int ExtWorker::setURL( const char * pURL )
{
    m_pConfig->setURL( pURL );
    return m_pConfig->updateServerAddr( pURL);
}


int ExtWorker::start()
{
    if ( m_iErrors > 10 )
        return -1;
    int ret = 0;
    ret = startEx();
    if ( ret == -1 )
    {
        LOG_WARN(( "[%s] Can not start this external application.",
            m_pConfig->getURL() ));
        setState( ST_BAD );
    }
    else
    {
        int max;
        if (( m_iState == ST_NOTSTARTED )||( ret == 0 ))
        {
            max = m_pConfig->getMaxConns();
            setState( ST_GOOD );
            m_iErrors = 0;
            if ( getConnPool().setMaxConns( max ) == -1 )
                return -1;
        }
        else
        {
            //max = m_pConfig->getMaxConns() - m_iErrors;
            //if ( max <= 0 )
                max = 1;
        }
    }
    return ret;
}

static int setToStop( void * p)
{
    ExtConn * pConn = (ExtConn *)(IConnection *)p;
    pConn->setToStop( 1 );
    return 0;
}

void ExtWorker::clearCurConnPool()
{
    ExtConn * pConn;
    while( (pConn = (ExtConn* )m_connPool.getFreeConn() ) != NULL )
    {
        pConn->close();
        m_connPool.removeConn( pConn );
    }
    m_iLingerConns += m_connPool.getTotalConns();
    m_connPool.for_each_conn( setToStop );
}

ExtConn* ExtWorker::getConn()
{
//    if ( D_ENABLED( DL_MEDIUM ) )
//        LOG_D(( "[%s] %d connections in connection pool!",
//                 m_pConfig->getURL(), getConnPool().getFreeConns() ));

    m_lIdleTime = 0;
    ExtConn* pConn = (ExtConn*) getConnPool().getFreeConn();
    if ( pConn )
    {
        if ( D_ENABLED( DL_LESS ) )
            LOG_D(( "[%s] connection available!",
                 m_pConfig->getURL() ));
    }
    else
    {
        if ( getConnPool().canAddMore() )
        {
            pConn = (ExtConn*)getConnPool().getBadConn();
            if ( pConn )
            {
                getConnPool().regConn( pConn );
            }
            else
            {
                pConn = newConn();
                if ( pConn )
                {
                    if ( D_ENABLED( DL_LESS ) )
                        LOG_D(( "[%s] create new connection succeed!",
                             m_pConfig->getURL() ));
                    getConnPool().regConn( pConn );
                    pConn->setWorker( this );
                }
            }
        }
    }
    return pConn;
}

void ExtWorker::recycleConn( ExtConn* pConn )
{
    if ( pConn->getToStop() )
    {
        m_iLingerConns--;
        pConn->close();
        m_connPool.removeConn( pConn );
        if ( !m_reqQueue.empty() )
            processPending();
        return;
    }
    while( !m_reqQueue.empty() )
    {

        ExtRequest * pReq =(ExtRequest *)m_reqQueue.pop_front();

        if ( !pReq->isAlive() )
        {
            if ( D_ENABLED( DL_LESS ) )
                LOG_D(( pReq->getLogger(), "[%s] Client side socket is closed, close connection!",
                     pReq->getLogId() ));
            continue;
        }
        if ( D_ENABLED( DL_LESS ) )
            LOG_D(( pReq->getLogger(), "[%s] assign pending request [%s] to recycled connection!",
                 m_pConfig->getURL(), pReq->getLogId() ));
        if ( pConn->assignReq( pReq ) == 0 )
            return;
        if ( pConn->getReq() )
            pConn->removeRequest( pReq );
        else
            return;  // pConn has been released by connectionError()

    }
    assert( m_reqQueue.size() == 0 );
//    //FIXME: debug code, should be removed later
//    if ( getConnPool().inFreeList( pConn ) )
//    {
//        LOG_ERR(( "[%s] Double count detected, connection is already in connection pool!",
//                m_pConfig->getURL() ));
//        return;
//    }
//    //end of debug code


    getConnPool().reuse( pConn );
    if ( D_ENABLED( DL_LESS ) )
        LOG_D(( "[%s] add recycled connection to connection pool!",
            m_pConfig->getURL() ));
}

int  ExtWorker::removeReq( ExtRequest * pReq )
{
    if ( D_ENABLED( DL_LESS ) )
        LOG_D(( "[%s] Request [%s] is removed from request queue!",
             m_pConfig->getURL(), pReq->getLogId() ));
    m_reqQueue.remove( pReq );
    return 0;
}


//return code:
//      0: OK
//      1: in the request queue
//     >1: Http status code
//

int  ExtWorker::processRequest( ExtRequest * pReq, int retry  )
{
    int ret;
    if ( m_iState == ST_NOTSTARTED )
        start();
    if ( m_iState == ST_BAD )
        return SC_503;
    ExtConn * pConn = NULL;
    if ( retry || m_reqQueue.empty() )
    {
        ExtConn * pConn = getConn();
        if ( pConn )
        {
            if ( D_ENABLED( DL_LESS ) )
                LOG_D(( "[%s] request [%s] is assigned with connection!",
                     m_pConfig->getURL(), pReq->getLogId() ));
            ret = pConn->assignReq( pReq );
            if ( ret )
            {
                if ( pConn->getReq() )
                {
                    pConn->removeRequest( pReq );
                    recycleConn( pConn );
                }
            }
            return ( ret > 0 ) ? ret : 0;
        }
    }
    if ( D_ENABLED( DL_LESS ) )
        LOG_D(( "[%s] connection unavailable, add new request [%s] "
                "to pending queue!",
             m_pConfig->getURL(), pReq->getLogId() ));
    pReq->suspend();
    if ( retry )
        m_reqQueue.push_front( pReq );
    else
    {
        m_reqQueue.append( pReq );
        if (( !pConn )&&( m_connPool.getTotalConns() - m_connPool.getFreeConns()
                            < m_connPool.getMaxConns() ))
            processPending();
    }
    return 0;
}


void ExtWorker::failOutstandingReqs()
{
    //if ( D_ENABLED( DL_LESS ) )
    LOG_INFO(( "[%s] Fail all outstanding requests!",
            m_pConfig->getURL() ));
    while( !m_reqQueue.empty() )
    {
        ExtRequest * pReq =(ExtRequest *)m_reqQueue.pop_front();
        if ( pReq->isAlive() )
        {
            if ( pReq->getLB() )
                pReq->tryRecover();
            else
                pReq->endResponse( SC_503, 0 );
        }
    }
}


void ExtWorker::processPending( )
{
    ExtConn * pConn = NULL;
    while( !m_reqQueue.empty() )
    {
        if ( !pConn )
        {
            pConn = getConn();
            if ( !pConn )
                return;
        }
        ExtRequest * pReq =(ExtRequest *)m_reqQueue.pop_front();
        if ( !pReq->isAlive() )
        {
            if ( D_ENABLED( DL_LESS ) )
                LOG_D(( pReq->getLogger(), "[%s] Client side socket is closed, close connection!",
                     pReq->getLogId() ));
            continue;
        }
        int ret = pConn->assignReq( pReq );
        if ( ret )
        {
            if ( pConn->getReq() )
            {
                pConn->removeRequest( pReq );
                recycleConn( pConn );
            }
            return;
        }
        else
            pConn = NULL;
        //if (( pReq->tryRecover() != 0 )&&( !incAttempt ))
        //    return;
    }
    if ( pConn )
        getConnPool().reuse( pConn );
        
}

int ExtWorker::connectionError( ExtConn * pConn, int errCode )
{
    ExtRequest * pReq = pConn->getReq();
    int ret = 0;
    if ( pReq )
    {
        pConn->removeRequest( pReq );
//        LOG_INFO(( "[%s] Connection error: %s, req: [%s]",
//            m_pConfig->getURL(), strerror( errCode ), pReq->getLogId() ));
    }
    if ( pConn->getToStop() )
        m_iLingerConns--;
    m_connPool.removeConn( pConn );
    if ( errCode == EAGAIN )
    {
        if ( pReq )
        {
            pReq->suspend();
            m_reqQueue.push_front( pReq );
        }
        return 1;
    }
    if ( !pConn->getReqProcessed() )
    {
        if ( D_ENABLED( DL_LESS ) )
            LOG_D(( "[%s] No Request has been processed successfully "
                    "through this connection, the maximum "
                    "connections allowed will be reduced!",
                 m_pConfig->getURL() ));

        //m_connPool.decMaxConn();
        //delete pConn;

        if (( errCode != ECONNRESET )&&( errCode != EPIPE )&&(errCode != ETIMEDOUT ))
        //if (( errCode == ECONNREFUSED)||( errCode == ETIMEDOUT ))
        {
            m_lErrorTime = time( NULL );
            if ( m_connPool.canAddMore() )
                m_connPool.decMaxConn();
            if (( errCode == ECONNREFUSED )||( errCode == ENOENT ))
            {
                while( (pConn = (ExtConn *)m_connPool.getFreeConn()) )
                {
                    pConn->close();
                    m_connPool.removeConn( pConn );
                }
                m_connPool.setMaxConns( 0 );
                m_lLastRestart = time( NULL );
                LOG_INFO(( "[%s] Connection refused, restart!",
                     m_pConfig->getURL() ));
                restart();
            }
            else
            {
                //m_connPool.adjustMaxConns();
                LOG_INFO(( "[%s] Connection error: %s, adjust "
                           "maximum connections to %d!",
                        m_pConfig->getURL(), strerror( errCode ),
                        m_connPool.getMaxConns() ));
                if ( m_connPool.getMaxConns() < (m_pConfig->getMaxConns() + 1) / 2 )
                {
                    int timeout = m_pConfig->getRetryTimeout();
                    if ( !timeout )
                        timeout = 10;
                    if (( m_lLastRestart )&&
                        ( time(NULL) - m_lLastRestart < timeout ))
                    {
                        LOG_NOTICE(( "[%s] Available connections are dropped below half "
                                 "of configured value:%d, restart too frequently, wait "
                                 "till timeout!", m_pConfig->getURL(),
                                 m_pConfig->getMaxConns() ));
                    }
                    else
                    {
                        LOG_NOTICE(( "[%s] Available connections are dropped below half "
                                 "of configured value:%d, start another group of external"
                                 " application!", m_pConfig->getURL(),
                                 m_pConfig->getMaxConns() ));
                        m_lLastRestart = time( NULL );
                        restart();
                    }
                }
            }
        }

        if ( m_connPool.getMaxConns() == 0 )
        {
            ++m_iErrors;
            //if ( m_iErrors > 10 )
            if ( m_pConfig->getRetryTimeout() > 0 )
            {
                LOG_WARN(( "[%s] All connections had gone bad, mark it "
                        "as bad and retry after a while!!!",
                    m_pConfig->getURL() ));
                stop();
                m_iState = ST_BAD;
                m_lErrorTime = time( NULL );
            }
            else
            {
                m_iState = ST_NOTSTARTED;
                m_iErrors = 0;
            }
        }
    }
    if ( pReq )
        ret = pReq->tryRecover();
    if ( m_iState == ST_BAD )
        failOutstandingReqs();
    return ret;
}

static int onConnTimer( void * p)
{
    ExtConn * pConn = (ExtConn *)(IConnection *)p;
    pConn->onSecTimer();
    return 0;
}


void ExtWorker::onTimer()
{
}

#include <http/httpvhost.h>
#include <stdio.h>
int ExtWorker::generateRTReport( int format, int wroteJsonExt, int fd, const char * pTypeName )
{
    char * p;
    char achBuf[4096];
    p = achBuf;
    detectDiedPid();
    m_connPool.for_each_conn( onConnTimer );
    m_reqStats.finalizeRpt();
    int inUseConn = m_connPool.getTotalConns() - m_connPool.getFreeConns();
    const HttpVHost * pVHost = m_pConfig->getVHost();
    if (( !pVHost || strcmp( pVHost->getName(), DEFAULT_ADMIN_SERVER_NAME) != 0 )&&
        ( m_connPool.getTotalConns() >= 0 )&&( m_iState != ST_NOTSTARTED ))
    
    {

        if(format == 0)
        {
                p += safe_snprintf( p, &achBuf[4096] - p,
                "EXTAPP [%s] [%s] [%s]: CMAXCONN: %d, EMAXCONN: %d, "
                "POOL_SIZE: %d, INUSE_CONN: %d, "
                "IDLE_CONN: %d, WAITQUE_DEPTH: %d, "
                "REQ_PER_SEC: %d, TOT_REQS: %d\n",
                pTypeName, (pVHost)?pVHost->getName():"", m_pConfig->getName(),
                m_pConfig->getMaxConns(), m_connPool.getMaxConns(),
                m_connPool.getTotalConns(), inUseConn,
                m_connPool.getFreeConns(), m_reqQueue.size(),
                m_reqStats.getRPS(), m_reqStats.getTotal() );
        }
        else if(format == 1)
        {
                p += safe_snprintf( p, &achBuf[4096] - p,
                "%s{\"TYPE\":\"%s\",\"VHOST\":\"%s\",\"NAME\":\"%s\",\"CMAX\":%d,\"EMAX\":%d,"
                "\"POOL\": %d,\"ACTIVE\":%d,"
                "\"IDLE\":%d,\"QUEUE\":%d,"
                "\"REQ_PER_SEC\":%d,\"TOT_REQ\":%d}\n",
                wroteJsonExt == 1 ? "," : "",
                pTypeName, (pVHost)?pVHost->getName():"", m_pConfig->getName(),
                m_pConfig->getMaxConns(), m_connPool.getMaxConns(),
                m_connPool.getTotalConns(), inUseConn,
                m_connPool.getFreeConns(), m_reqQueue.size(),
                m_reqStats.getRPS(), m_reqStats.getTotal() );

                wroteJsonExt = 1;
        }

        write( fd, achBuf, p - achBuf );
    }
    m_reqStats.reset();
    cleanStopPids();

    long lCurTime = DateTime::s_curTime;
    if ( m_iState == ST_BAD )
    {
        if ( lCurTime - m_lErrorTime > m_pConfig->getRetryTimeout() )
        {
            LOG_INFO(( "[%s] Error Timeout, restart and try again!",
                     m_pConfig->getURL() ));
            m_iState = ST_NOTSTARTED;
            m_iErrors = 0;
            m_lLastRestart = lCurTime;
            restart();
            processPending();
        }
    }
    else if (( m_pConfig->getMaxConns() > m_connPool.getMaxConns() )&&
             ( !m_reqQueue.empty() )&&( lCurTime - m_lErrorTime > 10 ))
    {
        m_connPool.setMaxConns( m_connPool.getMaxConns() + 1 );
        LOG_NOTICE(( "[%s] Increase effective max connections to %d.",
                    m_pConfig->getName(), m_connPool.getMaxConns() ));
        processPending();
    }
    else if ( ( !m_reqQueue.empty() )&& ( m_connPool.getMaxConns() > inUseConn ))
    {
        processPending();
    }
    //testing code
//    if ( m_reqStats.getTotal() > 100 )
//    {
//        LOG_NOTICE(( "[%s] Testing Restart.",
//                    m_pConfig->getName() ));
//        m_reqStats.resetTotal();
//        m_lLastRestart = time( NULL );
//        restart();
//    }

    if ((m_iState == ST_GOOD) && (m_connPool.getTotalConns() - m_connPool.getFreeConns() == 0))
    {
        if ( !m_lIdleTime )
            m_lIdleTime = lCurTime;
        if ( (int)(lCurTime - m_lIdleTime) >= m_pConfig->getMaxIdleTime() )
        {
            if ( D_ENABLED( DL_LESS ) )
                LOG_D(( "[%s] Max idle time reached, stop external application. ",
                     m_pConfig->getURL() ));
            stop();
        }
    }
    else
        m_lIdleTime = 0;

    //FIXME: add idle timeout
//    if ( stopWhenIdle() && (m_iState == ST_GOOD) && (m_connPool.getTotalConns() == 0) )
//    {
//        if ( D_ENABLED( DL_LESS ) )
//            LOG_D(( "[%s] Stop idle external application. ",
//                 m_pConfig->getURL() ));
//        stop();
//    }
    return wroteJsonExt;
}


#include <socket/coresocket.h>
#include <socket/gsockaddr.h>
#include <fcntl.h>
#include <unistd.h>

int ExtWorker::startServerSock( ExtWorkerConfig * pConfig, int backlog )
{
    int ret;
    int retry = 0;
    int fd = -1;
    const GSockAddr& addr = pConfig->getServerAddr();
    while( fd < 0)
    {
        ret = CoreSocket::listen( addr, backlog, &fd );
        if ( fd == -1 )
        {
            if ( ret == EINTR )
            {
                if ( D_ENABLED( DL_LESS ) )
                    LOG_D(( "[%s] listen() is interrupted, try again",
                            pConfig->getURL() ));
                
                continue;
            }
            char achBuf[256];
            addr.toString( achBuf, 256 );
            if ( ret == EADDRINUSE )
            {
                if ( ++retry < 10 )
                {
                    if (( retry == 9 )&&
                        ( addr.family() == PF_UNIX ))
                    {
                        LOG_NOTICE(( "Try to clean up unused unix sockets for [%s]",
                                    pConfig->getURL() ));
                        pConfig->removeUnusedSocket();
                    }
                    else
                        pConfig->altServerAddr();
                    continue;
                }
            }
            LOG_WARN(( "Can not listen on address[%s.*]: %s, please clean up manually!",
                        achBuf,
                        strerror( ret ) ));
            return -1;
        }
        fcntl( fd, F_SETFD, FD_CLOEXEC );
    }
    return fd;
}



