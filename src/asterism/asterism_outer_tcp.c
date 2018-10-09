#include "asterism_outer_tcp.h"
#include "asterism_core.h"
#include "asterism_utils.h"
#include "asterism_log.h"

static void outer_close_cb(
	uv_handle_t *handle)
{
	struct asterism_tcp_outer_s *outer = __CONTAINER_PTR(struct asterism_tcp_outer_s, socket, handle);
	AS_FREE(outer);
}

static void outer_close(
	struct asterism_tcp_outer_s *obj)
{
	asterism_handle_close((uv_handle_t *)&obj->socket);
}

static void incoming_close_cb(
	uv_handle_t *handle)
{
	int ret = 0;
	struct asterism_tcp_incoming_s *incoming = __CONTAINER_PTR(struct asterism_tcp_incoming_s, socket, handle);
	if (incoming->session) {
		RB_REMOVE(asterism_session_tree_s, &incoming->as->sessions, incoming->session);
		if(incoming->session->username) {
			AS_FREE(incoming->session->username);
		}
		if (incoming->session->password) {
			AS_FREE(incoming->session->password);
		}
		AS_FREE(incoming->session);
	}
	AS_FREE(incoming);
	asterism_log(ASTERISM_LOG_DEBUG, "tcp outer is closing");
}

static int parse_cmd_join(
	struct asterism_tcp_incoming_s *incoming, 
	struct asterism_trans_proto_s* proto)
{
	int offset = sizeof(struct asterism_trans_proto_s);
	unsigned short username_len = 0;
	char* username = 0;
	unsigned short password_len = 0;
	char* password = 0;

	//��ȡ�û�������
	if (offset + 2 > proto->len)
		return -1;
	username_len = ntohs(*(unsigned short*)((char*)proto + offset));
	offset += 2;

	if (offset + username_len > proto->len)
		return -1;
	username = (char*)((char*)proto + offset);
	offset += username_len;

	if (offset + 2 > proto->len)
		return -1;
	password_len = ntohs(*(unsigned short*)((char*)proto + offset));
	offset += 2;

	if (offset + password_len > proto->len)
		return -1;
	password = (char*)((char*)proto + offset);
	offset += password_len;

	//���û�������д�뵽�Ự�б�
	struct asterism_session_s* session = __ZERO_MALLOC_ST(struct asterism_session_s);
	session->username = as_strdup2(username, username_len);
	struct asterism_session_s* fs = RB_FIND(asterism_session_tree_s, &incoming->as->sessions, session);
	if (fs) {
		AS_FREE(session->username);
		AS_FREE(session);
		return -1;
	}
	session->password = as_strdup2(password, password_len);
	session->outer = (struct asterism_stream_s *)incoming;
	incoming->session = session;
	//��ʼ������tunnel����

	RB_INSERT(asterism_session_tree_s, &incoming->as->sessions, session);

	asterism_log(ASTERISM_LOG_DEBUG, "user: %s join.", session->username);

	return 0;
}

static void write_connect_ack_cb(
	uv_write_t *req,
	int status)
{
	free(req);
}

static int parse_cmd_connect_ack(
	struct asterism_tcp_incoming_s *incoming,
	struct asterism_trans_proto_s* proto)
{
	int offset = sizeof(struct asterism_trans_proto_s);
	//id
	if (offset + 4 > proto->len)
		return -1;
	unsigned int id = ntohl(*(unsigned int*)((char*)proto + offset));
	offset += 4;

	struct asterism_handshake_s fh = { id };
	struct asterism_handshake_s* handshake = RB_FIND(asterism_handshake_tree_s, &incoming->as->handshake_set, &fh);
	if (!handshake) {
		return -1;
	}
	RB_REMOVE(asterism_handshake_tree_s, &incoming->as->handshake_set, handshake);
	incoming->link = handshake->inner;
	incoming->link->link = (struct asterism_stream_s *)incoming;
	AS_FREE(handshake);

	//incoming->link

	//���http ok
	uv_write_t* req = __ZERO_MALLOC_ST(uv_write_t);
	req->data = incoming;
	uv_buf_t buf;
	buf.base = "HTTP/1.1 200 Connection Established\r\n\r\n";
	buf.len = sizeof("HTTP/1.1 200 Connection Established\r\n\r\n") - 1;

	int ret = uv_write( req, (uv_stream_t *)&incoming->link->socket, &buf, 1, write_connect_ack_cb );
	if (ret) {
		AS_FREE(req);
		return ret;
	}
	return 0;
}

static void write_cmd_pong_cb(
	uv_write_t *req,
	int status)
{
	struct asterism_tcp_incoming_s *incoming = (struct asterism_tcp_incoming_s *)req->data;
	AS_FREE(req);
}

static int parse_cmd_ping(
	struct asterism_tcp_incoming_s *incoming,
	struct asterism_trans_proto_s* proto)
{
	uv_write_t* req = __ZERO_MALLOC_ST(uv_write_t);
	req->data = incoming;
	uv_buf_t buf = uv_buf_init((char*)&_global_proto_pong, sizeof(_global_proto_pong));
	int ret = uv_write(req, (uv_stream_t *)&incoming->socket, &buf, 1, write_cmd_pong_cb);
	if (ret) {
		AS_FREE(req);
		return ret;
	}
	return 0;
}

static int incoming_parse_cmd_data(
	struct asterism_tcp_incoming_s *incoming,
	uv_buf_t *buf,
	int* eaten
)
{
	//���Ȳ���������ȡ
	if (buf->len < sizeof(struct asterism_trans_proto_s))
		return 0;
	struct asterism_trans_proto_s* proto = (struct asterism_trans_proto_s*)buf->base;
	uint16_t proto_len = ntohs(proto->len);
	if (proto->version != ASTERISM_TRANS_PROTO_VERSION)
		return -1;
	if (proto_len > ASTERISM_MAX_PROTO_SIZE)
		return -1;
	//���Ȳ���������ȡ
	if (proto_len > buf->len) {
		return 0;
	}
	//ƥ������
	if (proto->cmd == ASTERISM_TRANS_PROTO_JOIN) {
		asterism_log(ASTERISM_LOG_DEBUG, "connection join recv");
		if (parse_cmd_join(incoming, proto) != 0)
			return -1;
	}
	else if (proto->cmd == ASTERISM_TRANS_PROTO_CONNECT_ACK) {
		asterism_log(ASTERISM_LOG_DEBUG, "connection connect ack recv");
		if (parse_cmd_connect_ack(incoming, proto) != 0)
			return -1;
	}
	else if (proto->cmd == ASTERISM_TRANS_PROTO_PING) {
		//asterism_log(ASTERISM_LOG_DEBUG, "connection ping recv");
		if (parse_cmd_ping(incoming, proto) != 0)
			return -1;
	}
	else {
		return -1;
	}
	*eaten += proto_len;
	unsigned int remain = buf->len - proto_len;
	if (remain) {
		uv_buf_t __buf;
		__buf.base = buf->base + proto_len;
		__buf.len = remain;
		return incoming_parse_cmd_data(incoming, &__buf, eaten);
	}
	return 0;
}

static void incoming_read_cb(
	uv_stream_t *stream,
	ssize_t nread,
	const uv_buf_t *buf)
{
	struct asterism_tcp_incoming_s *incoming = __CONTAINER_PTR(struct asterism_tcp_incoming_s, socket, stream);
	int eaten = 0;

	uv_buf_t _buf;
	_buf.base = incoming->buffer;
	_buf.len = incoming->buffer_len;
	if (incoming_parse_cmd_data(incoming, &_buf, &eaten) != 0) {
		asterism_stream_close((struct asterism_stream_s*)incoming);
		return;
	}
	asterism_stream_eaten((struct asterism_stream_s*)incoming, eaten);
}

static void outer_accept_cb(
	uv_stream_t *stream,
	int status)
{
	int ret = ASTERISM_E_OK;
	//asterism_log(ASTERISM_LOG_DEBUG, "new tcp connection is comming");

	struct asterism_tcp_outer_s *outer = __CONTAINER_PTR(struct asterism_tcp_outer_s, socket, stream);
	struct asterism_tcp_incoming_s *incoming = 0;
	if (status != 0)
	{
		goto cleanup;
	}
	incoming = __ZERO_MALLOC_ST(struct asterism_tcp_incoming_s);
	ret = asterism_stream_accept(outer->as, stream, 0, incoming_read_cb, incoming_close_cb, (struct asterism_stream_s*)incoming);
	if (ret != 0)
	{
		ret = ASTERISM_E_FAILED;
		goto cleanup;
	}
	ret = asterism_stream_read((struct asterism_stream_s*)incoming);
	if (ret != 0)
	{
		ret = ASTERISM_E_FAILED;
		goto cleanup;
	}
cleanup:
	if (ret != 0)
	{
		asterism_stream_close((struct asterism_stream_s*)incoming);
	}
}

int asterism_outer_tcp_init(
	struct asterism_s *as,
	const char *ip, unsigned int *port)
{
	int ret = ASTERISM_E_OK;
	void *addr = 0;
	int name_len = 0;

	struct asterism_tcp_outer_s *outer = __ZERO_MALLOC_ST(struct asterism_tcp_outer_s);
	outer->as = as;
	ASTERISM_HANDLE_INIT(outer, socket, outer_close_cb);
	ret = uv_tcp_init(as->loop, &outer->socket);
	if (ret != 0)
	{
		asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
		ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
		goto cleanup;
	}
	addr = __ZERO_MALLOC_ST(struct sockaddr_in);
	name_len = sizeof(struct sockaddr_in);
	ret = uv_ip4_addr(ip, (int)*port, (struct sockaddr_in *)addr);

	if (ret != 0)
	{
		asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
		ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
		goto cleanup;
	}
	ret = uv_tcp_bind(&outer->socket, (const struct sockaddr *)addr, 0);
	if (ret != 0)
	{
		asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
		ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
		goto cleanup;
	}
	ret = uv_tcp_getsockname(&outer->socket, (struct sockaddr *)addr, &name_len);
	if (ret != 0)
	{
		asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
		ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
		goto cleanup;
	}
	*port = ntohs(((struct sockaddr_in *)addr)->sin_port);

	ret = uv_listen((uv_stream_t *)&outer->socket, ASTERISM_NET_BACKLOG, outer_accept_cb);
	if (ret != 0)
	{
		asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
		ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
		goto cleanup;
	}
cleanup:
	if (addr) {
		AS_FREE(addr);
	}
	if (ret)
	{
		outer_close(outer);
	}
	return ret;
}