#include "BaseSocket.h"
#include "EventDispatch.h"

//之所以不用map而用hash_map是因为STL的map底层是用红黑树实现的，查找时间复杂度是log(n)，
//而hash_map底层是用hash表存储的，查询时间复杂度是O(1)。
typedef hash_map<net_handle_t, CBaseSocket*> SocketMap;  
SocketMap	g_socket_map;

void AddBaseSocket(CBaseSocket* pSocket)
{
	g_socket_map.insert(make_pair((net_handle_t)pSocket->GetSocket(), pSocket));
}

void RemoveBaseSocket(CBaseSocket* pSocket)
{
	g_socket_map.erase((net_handle_t)pSocket->GetSocket());
}

CBaseSocket* FindBaseSocket(net_handle_t fd)
{
	CBaseSocket* pSocket = NULL;
	SocketMap::iterator iter = g_socket_map.find(fd);
	if (iter != g_socket_map.end())
	{
		pSocket = iter->second;
		pSocket->AddRef();
	}

	return pSocket;
}

//////////////////////////////

CBaseSocket::CBaseSocket()
{
	//log("CBaseSocket::CBaseSocket\n");
	m_socket = INVALID_SOCKET;
	m_state = SOCKET_STATE_IDLE;
}

CBaseSocket::~CBaseSocket()
{
	//log("CBaseSocket::~CBaseSocket, socket=%d\n", m_socket);
}

int CBaseSocket::Listen(const char* server_ip, uint16_t port, callback_t callback, void* callback_data)
{
	m_local_ip = server_ip;
	m_local_port = port;
	m_callback = callback;
	m_callback_data = callback_data;

	m_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (m_socket == INVALID_SOCKET)
	{
		printf("socket failed, err_code=%d\n", _GetErrorCode());
		return NETLIB_ERROR;
	}

	//将绑定的地址设置成reuse
	_SetReuseAddr(m_socket);
	//设置非阻塞socket
	_SetNonblock(m_socket);

	sockaddr_in serv_addr;
	_SetAddr(server_ip, port, &serv_addr);
    int ret = ::bind(m_socket, (sockaddr*)&serv_addr, sizeof(serv_addr));
	if (ret == SOCKET_ERROR)
	{
		log("bind failed, err_code=%d", _GetErrorCode());
		closesocket(m_socket);
		return NETLIB_ERROR;
	}

	ret = listen(m_socket, 64);
	if (ret == SOCKET_ERROR)
	{
		log("listen failed, err_code=%d", _GetErrorCode());
		closesocket(m_socket);
		return NETLIB_ERROR;
	}
	//将socket的状态设置成SOCKET_STATE_LISTENING，这个状态将侦听的socket与普通客户端连接的socket区别开来。
	m_state = SOCKET_STATE_LISTENING;

	log("CBaseSocket::Listen on %s:%d", server_ip, port);

	//AddBaseSocket(this);将socket句柄和对应的CBaseSocket放到一个全局对象中管理起来。
	AddBaseSocket(this);
	//目前只关注socket的读和异常事件，侦听socket可读意味着有新连接到来，异常就意味着侦听出错。对于服务器程序一般要关闭或重启服务
	CEventDispatch::Instance()->AddEvent(m_socket, SOCKET_READ | SOCKET_EXCEP);
	return NETLIB_OK;
}

net_handle_t CBaseSocket::Connect(const char* server_ip, uint16_t port, callback_t callback, void* callback_data)
{
	log("CBaseSocket::Connect, server_ip=%s, port=%d", server_ip, port);

	m_remote_ip = server_ip;
	m_remote_port = port;
	m_callback = callback;
	m_callback_data = callback_data;

	m_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (m_socket == INVALID_SOCKET)
	{
		log("socket failed, err_code=%d", _GetErrorCode());
		return NETLIB_INVALID_HANDLE;
	}

	_SetNonblock(m_socket);
	_SetNoDelay(m_socket);
	sockaddr_in serv_addr;
	_SetAddr(server_ip, port, &serv_addr);
	int ret = connect(m_socket, (sockaddr*)&serv_addr, sizeof(serv_addr));
	if ( (ret == SOCKET_ERROR) && (!_IsBlock(_GetErrorCode())) )
	{	
		log("connect failed, err_code=%d", _GetErrorCode());
		closesocket(m_socket);
		return NETLIB_INVALID_HANDLE;
	}
	m_state = SOCKET_STATE_CONNECTING;
	AddBaseSocket(this);
	CEventDispatch::Instance()->AddEvent(m_socket, SOCKET_ALL);
	
	return (net_handle_t)m_socket;
}

int CBaseSocket::Send(void* buf, int len)
{
	if (m_state != SOCKET_STATE_CONNECTED)
		return NETLIB_ERROR;

	int ret = send(m_socket, (char*)buf, len, 0);
	if (ret == SOCKET_ERROR)
	{
		int err_code = _GetErrorCode();
		//因为socket在创建的时候被设置成非阻塞的，所以，如果发送不了，底层send函数会立刻返回
		//并返回错误码EINPROGRESS（EWOULDBLOCK）表明对端tcp窗口太小，当前已经无法发出去：
		if (_IsBlock(err_code))
		{
#if ((defined _WIN32) || (defined __APPLE__))
			//这个时候关注socket的可写事件
			CEventDispatch::Instance()->AddEvent(m_socket, SOCKET_WRITE);
#endif
			ret = 0;
			//log("socket send block fd=%d", m_socket);
		}
		else
		{
			log("!!!send failed, error code: %d", err_code);
		}
	}

	return ret;
}

int CBaseSocket::Recv(void* buf, int len)
{
	return recv(m_socket, (char*)buf, len, 0);
}

int CBaseSocket::Close()
{
	CEventDispatch::Instance()->RemoveEvent(m_socket, SOCKET_ALL);
	RemoveBaseSocket(this);
	closesocket(m_socket);
	ReleaseRef();

	return 0;
}

void CBaseSocket::OnRead()
{
	//如果是listenfd则接受新的连接
	if (m_state == SOCKET_STATE_LISTENING)
	{
		_AcceptNewSocket();
	}
	else
	{	
		//connfd有事件到来，会回调imconCallback
		u_long avail = 0;
		//上述OnRead函数会走else分支，先调用ioctlsocket获得可读的数据字节数。如果出错或者字节数为0，
		//则以消息NETLIB_MSG_CLOSE调用回调函数imconn_callback,
		if ( (ioctlsocket(m_socket, FIONREAD, &avail) == SOCKET_ERROR) || (avail == 0) )
		{
			m_callback(m_callback_data, NETLIB_MSG_CLOSE, (net_handle_t)m_socket, NULL);
		}
		else
		{
			m_callback(m_callback_data, NETLIB_MSG_READ, (net_handle_t)m_socket, NULL);
		}
	}
}

void CBaseSocket::OnWrite()
{
#if ((defined _WIN32) || (defined __APPLE__))
	//可写，先移除可写事件
	CEventDispatch::Instance()->RemoveEvent(m_socket, SOCKET_WRITE);
#endif

	if (m_state == SOCKET_STATE_CONNECTING)
	{
		int error = 0;
		socklen_t len = sizeof(error);
#ifdef _WIN32

		getsockopt(m_socket, SOL_SOCKET, SO_ERROR, (char*)&error, &len);
#else
		getsockopt(m_socket, SOL_SOCKET, SO_ERROR, (void*)&error, &len);
#endif
		if (error) {
			m_callback(m_callback_data, NETLIB_MSG_CLOSE, (net_handle_t)m_socket, NULL);
		} else {
			m_state = SOCKET_STATE_CONNECTED;
			m_callback(m_callback_data, NETLIB_MSG_CONFIRM, (net_handle_t)m_socket, NULL);
		}
	}
	else
	{	
		//调用回调函数imconn_callback  在imconn。cpp中
		m_callback(m_callback_data, NETLIB_MSG_WRITE, (net_handle_t)m_socket, NULL);
	}
}

void CBaseSocket::OnClose()
{
	m_state = SOCKET_STATE_CLOSING;
	m_callback(m_callback_data, NETLIB_MSG_CLOSE, (net_handle_t)m_socket, NULL);
}

void CBaseSocket::SetSendBufSize(uint32_t send_size)
{
	int ret = setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &send_size, 4);
	if (ret == SOCKET_ERROR) {
		log("set SO_SNDBUF failed for fd=%d", m_socket);
	}

	socklen_t len = 4;
	int size = 0;
	getsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, &size, &len);
	log("socket=%d send_buf_size=%d", m_socket, size);
}

void CBaseSocket::SetRecvBufSize(uint32_t recv_size)
{
	int ret = setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &recv_size, 4);
	if (ret == SOCKET_ERROR) {
		log("set SO_RCVBUF failed for fd=%d", m_socket);
	}

	socklen_t len = 4;
	int size = 0;
	getsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, &size, &len);
	log("socket=%d recv_buf_size=%d", m_socket, size);
}

int CBaseSocket::_GetErrorCode()
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

bool CBaseSocket::_IsBlock(int error_code)
{
#ifdef _WIN32
	return ( (error_code == WSAEINPROGRESS) || (error_code == WSAEWOULDBLOCK) );
#else
	return ( (error_code == EINPROGRESS) || (error_code == EWOULDBLOCK) );
#endif
}

void CBaseSocket::_SetNonblock(SOCKET fd)
{
#ifdef _WIN32
	u_long nonblock = 1;
	int ret = ioctlsocket(fd, FIONBIO, &nonblock);
#else
	int ret = fcntl(fd, F_SETFL, O_NONBLOCK | fcntl(fd, F_GETFL));
#endif
	if (ret == SOCKET_ERROR)
	{
		log("_SetNonblock failed, err_code=%d", _GetErrorCode());
	}
}

void CBaseSocket::_SetReuseAddr(SOCKET fd)
{
	int reuse = 1;
	int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
	if (ret == SOCKET_ERROR)
	{
		log("_SetReuseAddr failed, err_code=%d", _GetErrorCode());
	}
}

void CBaseSocket::_SetNoDelay(SOCKET fd)
{
	int nodelay = 1;
	int ret = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
	if (ret == SOCKET_ERROR)
	{
		log("_SetNoDelay failed, err_code=%d", _GetErrorCode());
	}
}

void CBaseSocket::_SetAddr(const char* ip, const uint16_t port, sockaddr_in* pAddr)
{
	memset(pAddr, 0, sizeof(sockaddr_in));
	pAddr->sin_family = AF_INET;
	pAddr->sin_port = htons(port);
	pAddr->sin_addr.s_addr = inet_addr(ip);
	if (pAddr->sin_addr.s_addr == INADDR_NONE)
	{
		hostent* host = gethostbyname(ip);
		if (host == NULL)
		{
			log("gethostbyname failed, ip=%s", ip);
			return;
		}

		pAddr->sin_addr.s_addr = *(uint32_t*)host->h_addr;
	}
}

void CBaseSocket::_AcceptNewSocket()
{
	SOCKET fd = 0;
	sockaddr_in peer_addr;
	socklen_t addr_len = sizeof(sockaddr_in);
	char ip_str[64];
	while ( (fd = accept(m_socket, (sockaddr*)&peer_addr, &addr_len)) != INVALID_SOCKET )
	{
		//1. 产生一个新的socket和对应的CBaseSocket对象。
		CBaseSocket* pSocket = new CBaseSocket();
		uint32_t ip = ntohl(peer_addr.sin_addr.s_addr);
		uint16_t port = ntohs(peer_addr.sin_port);

		snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);

		log("AcceptNewSocket, socket=%d from %s:%d\n", fd, ip_str, port);

		pSocket->SetSocket(fd);
		pSocket->SetCallback(m_callback);
		pSocket->SetCallbackData(m_callback_data);
		//6. 将socket的状态设置成SOCKET_STATE_CONNECTED。
		pSocket->SetState(SOCKET_STATE_CONNECTED);
		pSocket->SetRemoteIP(ip_str);
		pSocket->SetRemotePort(port);

		//4. 禁用该socket的nagle算法（_SetNoDelay(fd);）。
		_SetNoDelay(fd);
		//3. 新socket同样被设置成非阻塞的。
		_SetNonblock(fd);
		//2. 该socket和对应的CBaseSocket对象和侦听socket一样也被加入全局g_socket_map中进行管理。
		AddBaseSocket(pSocket);
		//5. 关注该socket的读和异常事件
		CEventDispatch::Instance()->AddEvent(fd, SOCKET_READ | SOCKET_EXCEP);

		//7. 调用侦听socket的的回调函数m_callback(m_callback_data, NETLIB_MSG_CONNECT, (net_handle_t)fd, NULL)，并传入消息类型是NETLIB_MSG_CONNECT。
		//这个回调函数在上面初始化侦听函数设置的，指向main函数proxy_serv_callback
		m_callback(m_callback_data, NETLIB_MSG_CONNECT, (net_handle_t)fd, NULL);
	}
}

