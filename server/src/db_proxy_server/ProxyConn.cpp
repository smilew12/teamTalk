/*
 * ProxyConn.cpp
 *
 *  Created on: 2014年7月25日
 *      Author: ziteng
 */

#include "ProxyConn.h"
#include "ProxyTask.h"
#include "HandlerMap.h"
#include "atomic.h"
#include "IM.Other.pb.h"
#include "IM.BaseDefine.pb.h"
#include "IM.Server.pb.h"
#include "ThreadPool.h"
#include "SyncCenter.h"
static ConnMap_t g_proxy_conn_map;
static UserMap_t g_uuid_conn_map;
static CHandlerMap* s_handler_map;

uint32_t CProxyConn::s_uuid_alloctor = 0;
CLock CProxyConn::s_list_lock;
list<ResponsePdu_t*> CProxyConn::s_response_pdu_list;
static CThreadPool g_thread_pool;

void proxy_timer_callback(void* callback_data, uint8_t msg, uint32_t handle, void* pParam)
{
	uint64_t cur_time = get_tick_count();
	for (ConnMap_t::iterator it = g_proxy_conn_map.begin(); it != g_proxy_conn_map.end(); ) {
		ConnMap_t::iterator it_old = it;
		it++;

		CProxyConn* pConn = (CProxyConn*)it_old->second;
		pConn->OnTimer(cur_time);
	}
}

//
void proxy_loop_callback(void* callback_data, uint8_t msg, uint32_t handle, void* pParam)
{
	CProxyConn::SendResponsePduList();
}

/*
 * 用于优雅的关闭连接：
 * 服务器收到SIGTERM信号后，发送CImPduStopReceivePacket数据包给每个连接，
 * 通知消息服务器不要往自己发送数据包请求，
 * 然后注册4s后调用的回调函数，回调时再退出进程
 */
void exit_callback(void* callback_data, uint8_t msg, uint32_t handle, void* pParam)
{
	log("exit_callback...");
	exit(0);
}

static void sig_handler(int sig_no)
{
	if (sig_no == SIGTERM) {
		log("receive SIGTERM, prepare for exit");
        CImPdu cPdu;
        IM::Server::IMStopReceivePacket msg;
        msg.set_result(0);
        cPdu.SetPBMsg(&msg);
        cPdu.SetServiceId(IM::BaseDefine::SID_OTHER);
        cPdu.SetCommandId(IM::BaseDefine::CID_OTHER_STOP_RECV_PACKET);
        for (ConnMap_t::iterator it = g_proxy_conn_map.begin(); it != g_proxy_conn_map.end(); it++) {
            CProxyConn* pConn = (CProxyConn*)it->second;
            pConn->SendPdu(&cPdu);
        }
        // Add By ZhangYuanhao
        // Before stop we need to stop the sync thread,otherwise maybe will not sync the internal data any more
        CSyncCenter::getInstance()->stopSync();
        
        // callback after 4 second to exit process;
		netlib_register_timer(exit_callback, NULL, 4000);
	}
}

int init_proxy_conn(uint32_t thread_num)
{
	//将各个任务id和对应的处理函数绑定起来
	s_handler_map = CHandlerMap::getInstance();
	g_thread_pool.Init(thread_num);

	netlib_add_loop(proxy_loop_callback, NULL);

	signal(SIGTERM, sig_handler);

	return netlib_register_timer(proxy_timer_callback, NULL, 1000);
}

CProxyConn* get_proxy_conn_by_uuid(uint32_t uuid)
{
	CProxyConn* pConn = NULL;
	UserMap_t::iterator it = g_uuid_conn_map.find(uuid);
	if (it != g_uuid_conn_map.end()) {
		pConn = (CProxyConn *)it->second;
	}

	return pConn;
}

//////////////////////////
CProxyConn::CProxyConn()
{
	m_uuid = ++CProxyConn::s_uuid_alloctor;
	if (m_uuid == 0) {
		m_uuid = ++CProxyConn::s_uuid_alloctor;
	}

	g_uuid_conn_map.insert(make_pair(m_uuid, this));
}

CProxyConn::~CProxyConn()
{

}

void CProxyConn::Close()
{
	if (m_handle != NETLIB_INVALID_HANDLE) {
		netlib_close(m_handle);
		g_proxy_conn_map.erase(m_handle);

		g_uuid_conn_map.erase(m_uuid);
	}

	ReleaseRef();
}

void CProxyConn::OnConnect(net_handle_t handle)
{
	m_handle = handle;

	g_proxy_conn_map.insert(make_pair(handle, this));


	//已经悄悄地将该新socket的回调函数由proxy_serv_callback偷偷地换成了imconn_callback
	netlib_option(handle, NETLIB_OPT_SET_CALLBACK, (void*)imconn_callback);
	netlib_option(handle, NETLIB_OPT_SET_CALLBACK_DATA, (void*)&g_proxy_conn_map);
	netlib_option(handle, NETLIB_OPT_GET_REMOTE_IP, (void*)&m_peer_ip);
	netlib_option(handle, NETLIB_OPT_GET_REMOTE_PORT, (void*)&m_peer_port);

	log("connect from %s:%d, handle=%d", m_peer_ip.c_str(), m_peer_port, m_handle);
}

// 由于数据包是在另一个线程处理的，所以不能在主线程delete数据包，所以需要Override这个方法
//CImConn实际是代表一个连接，即每一个连接都有这样一个对象。具体被分化成它的各个子对象，
//如CProxyConn。每一个连接CImConn都存在一个读缓冲区和写缓冲区，读缓冲区用于存放从网络上收取的数据，
//写缓冲区用于存放即将发到网络中数据
void CProxyConn::OnRead()
{
	for (;;) {
		//先检测该对象的读缓冲区中还有多少可用空间，如果可用空间小于当前收到的字节数目，
		//则将该读缓冲区的大小扩展到需要的大小READ_BUF_SIZE
		uint32_t free_buf_len = m_in_buf.GetAllocSize() - m_in_buf.GetWriteOffset();
		if (free_buf_len < READ_BUF_SIZE)
			m_in_buf.Extend(READ_BUF_SIZE);
		//接着收到的数据放入读缓冲区中。并记录下这次收取数据的时间到m_last_recv_tick变量中。
		int ret = netlib_recv(m_handle, m_in_buf.GetBuffer() + m_in_buf.GetWriteOffset(), READ_BUF_SIZE);
		if (ret <= 0)
			break;

		m_recv_bytes += ret;
		m_in_buf.IncWriteOffset(ret);
		m_last_recv_tick = get_tick_count();
	}

	//接着开始解包，即调用CImPdu::IsPduAvailable()从读取缓冲区中取出数据处理
	uint32_t pdu_len = 0;
    try {
        while ( CImPdu::IsPduAvailable(m_in_buf.GetBuffer(), m_in_buf.GetWriteOffset(), pdu_len) ) {
			//得到包的大小就可以正式处理包了，
            HandlePduBuf(m_in_buf.GetBuffer(), pdu_len);
			//该包处理完成以后，将该包的数据从连接的读缓冲区移除：
            m_in_buf.Read(NULL, pdu_len);
        }
    } catch (CPduException& ex) {
        log("!!!catch exception, err_code=%u, err_msg=%s, close the connection ",
            ex.GetErrorCode(), ex.GetErrorMsg());
        OnClose();
    }
	
}


void CProxyConn::OnClose()
{
	Close();
}

void CProxyConn::OnTimer(uint64_t curr_tick)
{
	//发送心跳包m_last_recv_tick是上一次收取数据的时间，
	//如果当前时间举例上一次接收时间已经超过了指定的时间间隔（相当于一段时间内，对端没有给当前服务发送任何数据），
	//这个时候就关闭该连接（这里设置的时间间隔是30000毫秒，也就是30秒）
	if (curr_tick > m_last_send_tick + SERVER_HEARTBEAT_INTERVAL) {
        
        CImPdu cPdu;
        IM::Other::IMHeartBeat msg;
        cPdu.SetPBMsg(&msg);
        cPdu.SetServiceId(IM::BaseDefine::SID_OTHER);
        cPdu.SetCommandId(IM::BaseDefine::CID_OTHER_HEARTBEAT);
		SendPdu(&cPdu);
	}

	if (curr_tick > m_last_recv_tick + SERVER_TIMEOUT) {
		log("proxy connection timeout %s:%d", m_peer_ip.c_str(), m_peer_port);
		Close();
	}
}

void CProxyConn::HandlePduBuf(uchar_t* pdu_buf, uint32_t pdu_len)
{	
	//包的数据结构是CImPdu（Im 即Instant Message即时通讯软件的意思，teamtalk本来就是一款即时通讯，
	//pdu，Protocol Data Unit 协议数据单元，通俗的说就是一个包单位）
    CImPdu* pPdu = NULL;
    pPdu = CImPdu::ReadPdu(pdu_buf, pdu_len);
	//如果数据包是心跳包的话，就直接不处理了。因为心跳包只是来保活通信的，与具体业务无关：
    if (pPdu->GetCommandId() == IM::BaseDefine::CID_OTHER_HEARTBEAT) {
        return;
    }
    
	//接着根据对应的命令号调用在程序初始化阶段绑定的包处理函数：
    pdu_handler_t handler = s_handler_map->GetHandler(pPdu->GetCommandId());
    
	//执行处理函数不是直接调用该函数，而是包装成一个任务放入前面介绍的任务队列中
    if (handler) {
        CTask* pTask = new CProxyTask(m_uuid, handler, pPdu);

		//处理任务的线程可能有多个，那么到底将任务加入到哪个工作线程呢？这里采取的策略是随机分配：
        g_thread_pool.AddTask(pTask);
    } else {
        log("no handler for packet type: %d", pPdu->GetCommandId());
    }
}

/*
 * static method
 * add response pPdu to send list for another thread to send
 * if pPdu == NULL, it means you want to close connection with conn_uuid
 * e.g. parse packet failed
 */
void CProxyConn::AddResponsePdu(uint32_t conn_uuid, CImPdu* pPdu)
{
	ResponsePdu_t* pResp = new ResponsePdu_t;
	pResp->conn_uuid = conn_uuid;
	pResp->pPdu = pPdu;

	s_list_lock.lock();
	s_response_pdu_list.push_back(pResp);
	s_list_lock.unlock();
}

void CProxyConn::SendResponsePduList()
{
	s_list_lock.lock();
	while (!s_response_pdu_list.empty()) {
		ResponsePdu_t* pResp = s_response_pdu_list.front();
		s_response_pdu_list.pop_front();
		s_list_lock.unlock();
		//第一个就是一般服务器端会有多个连接对象，那么如何定位某个应答数据包对应的连接对象呢？
		//这里就通过数据包本身的conn_uuid来确定：
		CProxyConn* pConn = get_proxy_conn_by_uuid(pResp->conn_uuid);
		if (pConn) {
			if (pResp->pPdu) {
				pConn->SendPdu(pResp->pPdu);
			} else {
				log("close connection uuid=%d by parse pdu error\b", pResp->conn_uuid);
				pConn->Close();
			}
		}

		if (pResp->pPdu)
			delete pResp->pPdu;
		delete pResp;

		s_list_lock.lock();
	}

	s_list_lock.unlock();
}
