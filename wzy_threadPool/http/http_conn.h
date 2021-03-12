#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include <iostream>

#include "../lock/locker.h"
#include "../log/log.h"
#include "../CGImysql/sql_connection_pool.h"


class http_conn
{
public:
	/*文件名的最大长度*/
	static const int FILENAME_LEN = 200;
	/*读缓冲区的大小*/
	static const int READ_BUFFER_SIZE = 2048;
	/*写缓冲区的大小*/
	static const int WRITE_BUFFER_SIZE = 1024;
	/*HTTP请求的方法，目前仅支持GET*/
	enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATCH};
	/*解析客户请求时，主状态机所处的状态,CHECK_STATE_REQUESTLINE：正在分析请求行，CHECK_STATE_HEADER：正在分析请求头， CHECK_STATE_CONNECT：正在分析请求体*/
	enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONNECT};
	/*服务器处理HTTP请求的可能结果，NO_REQUEST：请求不完整, GET_REQUEST：获得了完整请求, BAD_REQUEST：请求语法错误, NO_RESOURCE：客户无法访问资源
	, FORBIDDEN_REQUEST：禁止访问, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION*/
	enum HTTP_CODE {NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};
	/*行的读取状态，从状态机	LINE_OK：完整读取一行 LINE_BAD：报文语法有误 LINE_OPEN：读取的行不完整*/
	enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN};
public:
	http_conn() {}
	~http_conn() {}

public:
	/*初始化接受新的连接，内部会调用私有方法init*/
	void init(int sockfd, const sockaddr_in &addr);
	/*关闭连接*/
	void close_conn(bool real_close = true);
	/*处理客户请求*/
	void process();
	/*读取浏览器端发来的全部数据*/
	bool read();
	/*响应报文写入函数*/
	bool write();
	// 返回客户端信息
    sockaddr_in *get_address() { return &m_address; }
	// 初始化数据库,即把数据库数据放到map中
	void initmysql_result(connection_pool *connPool);
private:
	/*初始化连接*/
	void init();
	/*从m_read_buf读取，并处理请求报文*/
	HTTP_CODE process_read();
	/*向m_write_buf写入响应报文数据*/
	bool process_write(HTTP_CODE ret);

	/*下面一组函数用于process_read调用，分析HTTP请求*/
	//主状态机解析报文中的请求行数据
	HTTP_CODE parse_request_line(char* text);
	//主状态机解析报文中的请求头数据
	HTTP_CODE parse_headers(char* text);
	//主状态机解析报文中的请求内容
	HTTP_CODE parse_content(char* text);
	 //生成响应报文
	HTTP_CODE do_request();
	//m_start_line是已经解析的字符，m_read_buf是读缓冲区的初始位置。 get_line用于将指针向后偏移，指向未处理的字符
	char* get_line() {return m_read_buf + m_start_line;}
	//从状态机读取一行，分析是请求报文的哪一部分
	LINE_STATUS parse_line();

	/*下面这组函数用于process_write调用，填充http应答*/
	void unmap();
	bool add_response(const char* format, ...);
	bool add_content(const char* content);
	bool add_status_line(int status, const char* title);
	bool add_headers(int content_length);
	bool add_content_length(int content_length);
	bool add_linger();
	bool add_blank_line();
	bool add_content_type();

public:
	/*所有socket事件被注册到同一个epoll内核事件表中，所以将epoll文件描述符设置为静态的*/
	static int m_epollfd;
	/*统计用户数量*/
	static int m_user_count;
	// 数据库连接池
	MYSQL *mysql;
private:
	/*该http连接的socket和对方的socket地址*/
	int m_sockfd;
	sockaddr_in m_address;

	/*读缓冲区*/
	char m_read_buf[READ_BUFFER_SIZE];
	/*读缓冲区中最后一个字节的下一位置*/
	int m_read_idx;
	/*当前分析的字符在读缓存区的位置*/
	int m_check_idx;
   	//行在buffer中的起始位置
	int m_start_line;
	/*写缓冲区*/
	char m_write_buf[WRITE_BUFFER_SIZE];
	/*写缓冲区已经写入待发送的字符数*/
	int m_write_idx;
	/*主状态机所处的状态*/
	CHECK_STATE m_check_state;
	/*请求方法*/
	METHOD m_method;
	/*客户请求的目标文件完整路径，其内容等于doc_root+m_url,doc_root是网站根目录*/
	char m_real_file[FILENAME_LEN];
	/*客户请求目标文件的名字*/
	char* m_url;
	/*http协议的版本号,仅支持http1.1*/
	char* m_version;
	/*主机名字*/
	char* m_host;
	/*http消息体的长度*/
	int m_content_length;
	/*http请求是否要求保持连接*/
	bool m_linger;
	//读取服务器上的文件地址
	char* m_file_address;
	/*目标文件的状态。判断文件是否存在，是否为目录，是否可读，获取文件大小*/
	struct stat m_file_stat;
	/*采用writev来执行写操作，m_iv_count表示被写内存块的数量*/
	struct iovec m_iv[2];
	int m_iv_count;
	// 是否启用cgi的POST方法
	int cgi;
	//存储请求头数据
	char *m_string;   
	//剩余发送字节数             
	int bytes_to_send;  
	//已发送字节数        
	int bytes_have_send;        
};

#endif
