#include "http_conn.h"
#include <unordered_map>
#include <mysql/mysql.h>
#include <fstream>
//#define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

//#define listenfdET //边缘触发非阻塞
//#define listenfdLT //水平触发阻塞

/*定义一些HTTP的响应状态*/
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad sytex or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

/*网站的根目录*/
char* doc_root;


// 将表中的用户名和密码放入map，注意是全局变量
unordered_map<string, string> users;
locker m_lock;

// 这里有个缺陷是每个http连接都需要初始化一个users的哈希表，如果连接数很多占用内存过大
void http_conn::initmysql_result(connection_pool *connPool)
{
	const char *a = "/root"
	getcwd(doc_root, 100);
	stract(doc_root, a);
	// 1. 从连接池取出一个连接
	MYSQL *mysql = nullptr;
	// connectionRAII构造函数中从connPool连接池取出了一个连接对mysql初始化，初始化结束后connectionRAII对象析构，释放连接
	connectionRAII mysqlcon(&mysql, connPool);

	// 在user表中检索username，password数据，通过浏览器输入
	if(mysql_query(mysql, "SELECT username,password FROM user"))
	{
		LOG_ERROR("SELECT error : %s\n", mysql_error(mysql));
	}
	// 从表中检索完整的结果集合
	MYSQL_RES *result = mysql_store_result(mysql);
	// 返回结果集合中的列数
	int num_fields = mysql_num_fields(result);
	// 返回所有字段结构的数组
	MYSQL_FIELD *fields = mysql_fetch_fields(result);
	// 从结果集合中获取下一行，将对应的用户名和密码存入map
	while(MYSQL_ROW row = mysql_fetch_row(result))
	{
		string temp1(row[0]);
		string temp2(row[1]);
		users[temp1] = temp2;
	}
}

int setnonblocking(int fd)
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

void addfd(int epollfd, int fd, bool one_shot)
{
	epoll_event event;
	event.data.fd = fd;

	#ifdef connfdET
		event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	#endif

	#ifdef connfdLT
		event.events = EPOLLIN | EPOLLRDHUP;
	#endif

	#ifdef listenfdET
		event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	#endif

	#ifdef listenfdLT
		event.events = EPOLLIN | EPOLLRDHUP;
	#endif

	if(one_shot)
	{
		event.events |= EPOLLONESHOT;
	}
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
	setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
	close(fd);
}

//起的作用类似于add，区别在于mod针对已经注册在描述符表的描述符修改
void modfd(int epollfd, int fd, int ev)
{
	epoll_event event;
	event.data.fd = fd;

	#ifdef connfdET
		event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
	#endif

	#ifdef connfdLT
		event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
	#endif
	epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
	if(real_close && (m_sockfd != -1))
	{
		removefd(m_epollfd, m_sockfd);
		m_sockfd = -1;
		m_user_count--;
	}
}

void http_conn::init(int sockfd, const sockaddr_in& addr)
{
	m_sockfd = sockfd;
	m_address = addr;

	addfd(m_epollfd, sockfd, true);
	m_user_count++;
	init();
}

void http_conn::init()
{
	m_check_state = CHECK_STATE_REQUESTLINE;
	m_linger = false;

	m_method = GET;
	m_url = 0;
	m_version = 0;
	m_content_length = 0;
	m_host = 0;
	m_start_line = 0;
	m_check_idx = 0;
	m_read_idx = 0;
	m_write_idx = 0;
	memset(m_read_buf, '\0', READ_BUFFER_SIZE);
	memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
	memset(m_real_file, '\0', FILENAME_LEN);
}

/*从状态机，用于解析一行的内容*/
http_conn::LINE_STATUS http_conn::parse_line()
{
	char temp;
	/*check_idx获取指向read_buf读缓冲区中正在分析的字节， read_idx指向缓冲区中
	已经读入客户数据的尾部(指向所有数据的尾部，所以是for循环终止条件)的下一字节。
	read_buff中0~check_idx字节已经分析完毕，第chech_idx~ (read_idx-1)  由下面循环挨个分析*/
	for(; m_check_idx < m_read_idx; ++m_check_idx)
	{
		/*当前要分析的字节*/
		temp = m_read_buf[m_check_idx];
		/*回车符 '\r'*/ 
		if(temp == '\r')
		{/*如果回车符碰巧是buffer中最后一个已经被读入的客户数据，那么这次分析没有读到一个完整行
		返回LINE_OPEN表示还需要继续读取客户数据才能进一步分析  */
			if((m_check_idx+1) == m_read_idx)
			{
				return LINE_OPEN;
			}
			/*如果下一个字符是'\n'换行符，这说明我们已经读入一个完整行了*/
			else if(m_read_buf[m_check_idx+1] == '\n')
			{
				m_read_buf[m_check_idx++] = '\0';
				m_read_buf[m_check_idx++] = '\0';
				return LINE_OK;
			}
			// 两种情况都不是，这说明请求出了问题
			return LINE_BAD;
		}
		// 如果当前字节是'\n'这说明也读到了完整行
		else if(temp == '\n')
		{
			if(m_check_idx > 1 && (m_read_buf[m_check_idx-1]) == '\r')
			{
				m_read_buf[m_check_idx-1] = '\0';
				m_read_buf[m_check_idx++] = '\0';
				return LINE_OK;
			}
			// 同上
			return LINE_BAD;
		}
	}
	// 如果所有都读完，没碰到'\r'，这说明还要继续读
	return LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
	if(m_read_idx >= READ_BUFFER_SIZE)
	{
		return false;
	}

	int byte_read = 0;

	while(true)
	{//m_read_idx是读缓冲区未用的第一个位置，因此recv从 此处开始读入数据，读入长度为剩余空间(READ_BUFFER_SIZE-m_read_idx)
		byte_read = recv(m_sockfd, m_read_buf+m_read_idx, READ_BUFFER_SIZE-m_read_idx, 0);

		if(byte_read == -1)
		{//EAGIN和EWOULDBLOCK一样的含义，都是非阻塞操作没有读到数据直接返回设置的errno
			if(errno == EAGAIN || errno == EWOULDBLOCK)
			{
				break;
			}
			return false;
		}
		else if(byte_read == 0)
		{
			return false;
		}
		//修改m_read_idx的读取字节数
		m_read_idx += byte_read;
	}
	return true;
}

http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
	//请求行中最先含有空格和\t任一字符的位置并返回
	m_url = strpbrk(text, " \t");//这里在text中检测"\t"目的是找到url的前一位置，此时GET位于text中
	if(!m_url)
	{
		return BAD_REQUEST;
	}

	//将该位置改为\0，用于将前面数据取出
	*m_url++ = '\0';

	//解析请求方法GET
	char* method = text;
	if(strcasecmp(method, "GET") == 0)
	{
		m_method = GET;
	}
	else if(strcasecmp(method, "POST") == 0)
	{
		m_method = POST;
		cgi = 1;
	}
	else
	{
		return BAD_REQUEST;
	}

	//此时m_url包含url和version两部分
	//去除m_url头部的空行
	m_url += strspn(m_url, " \t");

	//找到version和url的分割点,同上
	m_version = strpbrk(m_url, " \t");
	if(!m_version)
	{
		return BAD_REQUEST;
	}
	*m_version++ = '\0';//进行url和version分割
	m_version += strspn(m_version, " \t");//去除version头部的空行

	if(strcasecmp(m_version, "HTTP/1.1") != 0)
	{
		return BAD_REQUEST;
	}

	//if目的为了去除m_url头部的 http://
	if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
		//m_url进入真正url位置
        m_url = strchr(m_url, '/');
    }

	//针对https情况
	if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
		//m_url进入真正url位置
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
	{
		return BAD_REQUEST;
	}
    // 当url为/时，显示欢迎界面
	if(strlen(m_url) == 1)
	{
		strcat(m_url, "judge.html");
	}

	//请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析http请求的一个头部信息,以 关键字：内容为格式
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
	// 遇到空行，表示头部字段解析完毕
	if(text[0] == '\0')
	{
		// 如果http请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
		if(m_content_length != 0)
		{
			m_check_state = CHECK_STATE_CONNECT;
			return NO_REQUEST;
		}
		// 否之说明已经得到了一个完整的http请求
		return GET_REQUEST;
	}
	// 解析请求头部连接字段
	else if(strncasecmp(text, "Connection:", 11) == 0)
	{
		text += 11;
		//跳过空格和\t字符
		text += strspn(text, " \t");
		if(strcasecmp(text, "keep-alive") == 0)
		{
			//如果是长连接，则将linger标志设置为true
			m_linger = true;
		}
	}
	// 解析请求头部内容长度字段
	else if(strncasecmp(text, "Content-Length:", 15) == 0)
	{
		text += 15;
		text += strspn(text, " \t");
		m_content_length = atol(text);
	}
	// 处理host头部字段
	else if(strncasecmp(text, "Host:", 5) == 0)
	{
		text += 5;
		text += strspn(text, " \t");
		m_host = text;
	}
	else
	{
		LOG_INFO("oop!unknow header: %s\n", text);
        Log::get_instance()->flush();
	}
	return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它有没有读入
// 注意我们只是读到请求头末端，m_check_idx相当于位于请求体的首端，因此m_content_length+m_check_idx相当于包含了整个请求体
// m_read_idx超过则表明请求体被完整读入了
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
	if(m_read_idx >= (m_content_length+m_check_idx))
	{
		text[m_content_length] = '\0';
		// POST请求中最后为输入的用户名和密码
		m_string = text;
		return GET_REQUEST;
	}
	return NO_REQUEST;
}

// 主状态机，
http_conn::HTTP_CODE http_conn::process_read()
{
	LINE_STATUS line_staus = LINE_OK;
	HTTP_CODE ret = NO_REQUEST;
	char* text = 0;

	while((m_check_state == CHECK_STATE_CONNECT) && (line_staus == LINE_OK) || ((line_staus = parse_line()) == LINE_OK))
	{
		text = get_line();
        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
		m_start_line = m_check_idx;

		LOG_INFO("got a http line : %s\n", text);
        Log::get_instance()->flush();
		//主状态机的三种状态转移逻辑
		switch (m_check_state)
		{
		case CHECK_STATE_REQUESTLINE:
		{
			ret = parse_request_line(text);
			if(ret == BAD_REQUEST)
			{
				return BAD_REQUEST;
			}
			break;
		}
		case CHECK_STATE_HEADER:
		{
			ret = parse_headers(text);
			if(ret == BAD_REQUEST)
			{
				return BAD_REQUEST;
			}
			//完整解析GET请求后，跳转到报文响应函数
			else if(ret == GET_REQUEST)
			{
				return do_request();
			}
			break;
		}	
		case CHECK_STATE_CONNECT:
		{
			//解析消息体
			ret = parse_content(text);
			if(ret == GET_REQUEST)
			{
				return do_request();
			}
			//解析完消息体即完成报文解析，避免再次进入循环，更新line_status
			line_staus = LINE_OPEN;
			break;
		}
		default:
			return INTERNAL_ERROR;
		}
	}
	return NO_REQUEST;
}

// 当得到一个完整、正确的http请求时，对目标文件属性进行分析，如果文件存在，则对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处
// 并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
	//将初始化的m_real_file赋值为网站根目录
	strcpy(m_real_file, doc_root);
	int len = strlen(doc_root);
	// 查找一个字符c在另一个字符串str中末次出现的位置（也就是从str的右侧开始查找字符c首次出现的位置），并返回这个位置的地址。
	// 如果未能找到指定字符，那么函数将返回NULL。使用这个地址返回从最后一个字符c到str末尾的字符串。
	const char *p = strrchr(m_url, '/');

	// 处理cgi
	if(cgi == 1 && (*(p+1) == '2' || *(p+1) == '3'))
	{
		// 根据标志判断是登录检测还是注册检测
		char flag = m_url[1];

		char *m_url_real = (char *)malloc(sizeof(char)*200);
		strcpy(m_url_real, "/");
		strcat(m_url_real, m_url+2);

		strncpy(m_real_file+len, m_url_real, FILENAME_LEN-len-1);
		free(m_url_real);

		// 提取用户名和密码，例如user=123&passwd=123
		char name[100], password[100];
		int i;
		for(i = 5; m_string[i] != '&'; ++i)
		{
			name[i-5] = m_string[i];
		}
		name[i-5] = '\0';

		int j = 0;
		for(i = i+10; m_string[i] != '\0'; ++i, ++j)
		{
			password[j] = m_string[i];
		}
		password[j] = '\0';

		// 同步线程登录校验
		if(*(p+1) == '3')
		{
			// *(p+1) == '3'表示注册
			// 如果是注册，先检测数据库重名
			// 没有重名增加数据
			char *sql_insert = (char*)malloc(sizeof(char)*200);
			strcpy(sql_insert, "INSERT INTO user(username, password) VALUES(");
			strcat(sql_insert, "'");
			strcat(sql_insert, name);
			strcat(sql_insert, "', '");
			strcat(sql_insert, password);
			strcat(sql_insert, "')");
			// 注意char *数据可以直接赋给string，自动转换
			if(users.find(name) == users.end())
			{
				// 用户名没注册过则进行注册
				m_lock.lock();
				// int res = mysql_query(&conn,"SQL语句");
				// if(!res)表示成功
				// 如果请求成功将数据写入MYSQL_RES result;
				// 如果result不为空
				int res = mysql_query(mysql, sql_insert);
				users[name] = password;
				m_lock.unlock();
				if(!res)
				{
					// 表示语句成功
					strcpy(m_url, "/log.html");
				}
				else
				{
					strcpy(m_url, "/registerError.html");
				}
			}
			else
			{
				// 表示名字重复，注册失败
				strcpy(m_url, "/registerError.html");
			}
		}
		// *(p+1) == '2'表示登录直接判断
		// 如果浏览器输入的用户名和密码能在表里找到，返回1.否之返回0
		else if(*(p+1) == '2')
		{
			if(users.empty())
			{
				cout << "map is empty" << endl;
			}
			if(users.find(name) != users.end() && users[name] == password)
			{
				cout << "passwd success" << endl;
				strcpy(m_url, "/welcome.html");
			}
			else
			{
				strcpy(m_url, "/logError.html");
			}
		}
	}
	// 如果请求资源为0，跳转注册界面
	if (*(p + 1) == '0')
	{
		char *m_url_real = (char *)malloc(sizeof(char) * 200);
		strcpy(m_url_real, "/register.html");
		strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

		free(m_url_real);
	}
	// 请求资源为1，登录界面
	else if (*(p + 1) == '1')
	{
		char *m_url_real = (char *)malloc(sizeof(char) * 200);
		strcpy(m_url_real, "/log.html");
		strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

		free(m_url_real);
	}
	else if (*(p + 1) == '5')
	{
		char *m_url_real = (char *)malloc(sizeof(char) * 200);
		strcpy(m_url_real, "/picture.html");
		strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

		free(m_url_real);
	}
	else if (*(p + 1) == '6')
	{
		char *m_url_real = (char *)malloc(sizeof(char) * 200);
		strcpy(m_url_real, "/video.html");
		strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

		free(m_url_real);
	}
	else if (*(p + 1) == '7')
	{
		char *m_url_real = (char *)malloc(sizeof(char) * 200);
		strcpy(m_url_real, "/fans.html");
		strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

		free(m_url_real);
	}		
	else
	{
		//碰到 '\0'会自动结束copy
		strncpy(m_real_file+len, m_url, FILENAME_LEN-len-1);
	}

	//通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
	//失败返回NO_RESOURCE状态，表示资源不存在
	if(stat(m_real_file, &m_file_stat) < 0)
	{
		return NO_RESOURCE;
	}
	/* st_mode:文件的类型和存取的权限	S_IROTH:其他用户具可读取权限*/ 
	if(!(m_file_stat.st_mode & S_IROTH))
	{
		return FORBIDDEN_REQUEST;
	}
	//判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
	if(S_ISDIR(m_file_stat.st_mode))
	{
		return BAD_REQUEST;
	}
	//以只读方式获取文件描述符，通过mmap将该文件映射到内存中
	int fd = open(m_real_file, O_RDONLY);
	// 将文件映射到虚拟空间内存地址，起始位置为m_real_file
	m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	//避免文件描述符的浪费和占用
	close(fd);
	//表示请求文件存在，且可以访问
	return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap()
{
	if(m_file_address)
	{
		munmap(m_file_address, m_file_stat.st_size);
		m_file_address = 0;
	}
}

// 写http响应
bool http_conn::write()
{
	int temp = 0;
	int newadd = 0;

	bytes_have_send = 0;
	bytes_to_send = m_write_idx;
	for(int i = 0; i < 2; ++i)
	bytes_to_send += int(m_iv[i].iov_len);

	//若要发送的数据长度为0,表示响应报文为空，一般不会出现这种情况
	if(bytes_to_send == 0)
	{
		modfd(m_epollfd, m_sockfd, EPOLLIN);
		init();
		return true;
	}

	while(1)
	{
		// struct iovec m_iv[2];把散步的数据同时写，返回temp表示已经写的数据，由于非阻塞，能写多少就写多少
		temp = writev(m_sockfd, m_iv, m_iv_count);
		if(temp <= -1)
		{
			// 如果TCP写缓冲区没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，服务器无法立即接收到同一客户端的下一请求
			// 但是这可以保证连接的完整性
			if(errno == EAGAIN)
			{
                //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
                if (bytes_have_send >= m_iv[0].iov_len)
                {
                	//不再继续发送头部信息
                    m_iv[0].iov_len = 0;
                    m_iv[1].iov_base = m_file_address + newadd;
                    m_iv[1].iov_len = bytes_to_send;
               	}
                //继续发送第一个iovec头部信息的数据
                else
                {
                	m_iv[0].iov_base = m_write_buf + bytes_to_send;
                   	m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
                }

				//重新注册写事件
				modfd(m_epollfd, m_sockfd, EPOLLOUT);
				return true;
			}
			unmap();
			return false;
		}

		//更新已发送字节数
		bytes_to_send -= temp;
		//更新已发送字节
		bytes_have_send += temp;
		// 注意到bytes_have_send包括两部分，一为响应报文，最大长度为m_write_idx
		// 二为需要发送的文件，因此下面的newadd表示文件已经发送大小
		// 偏移文件iovec的指针(表示文件已经发送多少字节)
		newadd = bytes_have_send - m_write_idx;
		if(bytes_to_send <= 0)//修改
		{
			// 发送HTTP响应成功，根据HTTP请求中Connection字段决定是否关闭连接
			unmap();
			modfd(m_epollfd, m_sockfd, EPOLLIN);
			if(m_linger)
			{
				init();
				return true;
			}
			else
			{
				return false;
			}
		}
	}
}
// 往缓冲区写入待发送的数据
bool http_conn::add_response(const char* format, ...)
{
	//如果写入内容超出m_write_buf大小则报错
	if(m_write_idx >= WRITE_BUFFER_SIZE)
	{
		return false;
	}
	//定义可变参数列表
	va_list arg_list;

	 //将变量arg_list初始化为传入参数
	va_start(arg_list, format);
	//将数据format从可变参数列表写入缓冲区写，返回写入数据的长度（间隔符逗号计算在内）
	int len = vsnprintf(m_write_buf+m_write_idx, WRITE_BUFFER_SIZE-1-m_write_idx, format, arg_list);

	//如果写入的数据长度超过缓冲区剩余空间，则报错
	if(len >= (WRITE_BUFFER_SIZE-1-m_write_idx))
	{
		va_end(arg_list);
		return false;
	}
	//更新m_write_idx位置
	m_write_idx += len;
	//清空可变参列表
	va_end(arg_list);
    LOG_INFO("response : %s\n", m_write_buf);
    Log::get_instance()->flush();
	return true;
}
// 加入状态行
bool http_conn::add_status_line(int status, const char* title)
{
	return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
// 加入请求头
bool http_conn::add_headers(int content_len)
{
	add_content_length(content_len);
	add_linger();
	add_blank_line();
	return true;
}
// 加入请求内容长度
bool http_conn::add_content_length(int content_len)
{
	return add_response("Content-Length:%d\r\n", content_len);
}
// 加入连接状态
bool http_conn::add_linger()
{
	return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
// 加入空白行
bool http_conn::add_blank_line()
{
	return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content)
{
	return add_response("%s", content);
}

// 根据服务器处理HTTP的请求，决定返回客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
	switch (ret)
	{
		//内部错误，500
		case INTERNAL_ERROR:
		{
			add_status_line(500, error_500_title);
			add_headers(strlen(error_500_form));
			if(!add_content(error_400_form))
			{
				return false;
			}
			break;
		}
		//资源没有访问权限，403
		case FORBIDDEN_REQUEST:
		{
			add_status_line(403, error_403_title);
			add_headers(strlen(error_403_form));
			if(!add_content(error_403_form))
			{
				return false;
			}
			break;
		}
		//报文语法有误，400
		case BAD_REQUEST:
		{
			add_status_line(400, error_400_title);
			add_headers(strlen(error_400_form));
			if(add_content(error_400_form))
			{
				return false;
			}
			break;
		}
		//没有请求资源，404
		case NO_RESOURCE:
		{
			add_status_line(404, error_404_title);
			add_headers(strlen(error_404_form));
			if(add_content(error_404_form))
			{
				return false;
			}
			break;
		}
		//文件存在，200
		case FILE_REQUEST:
		{
			add_status_line(200, ok_200_title);
			if(m_file_stat.st_size != 0)
			{
				add_headers(m_file_stat.st_size);
				//第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
				m_iv[0].iov_base = m_write_buf;
				m_iv[0].iov_len = m_write_idx;
				//第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
				m_iv[1].iov_base = m_file_address;
				m_iv[1].iov_len = m_file_stat.st_size;
				m_iv_count = 2;
				bytes_to_send = m_write_idx+m_file_stat.st_size;
				return true;
			}
			else
			{
				//如果请求的资源大小为0，则返回空白html文件
				const char *ok_string = "<html><body></body></html>";
				add_headers(strlen(ok_string));
				if(!add_content(ok_string))
				{
					return false;
				}
			}
		}
		default:
		{
			return false;
		}
	}
	//除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
	m_iv[0].iov_base = m_write_buf;
	m_iv[0].iov_len = m_write_idx;
	m_iv_count = 1;
	return true;
}

// 由线程池中的工作线程调用，处理HTTP请求的入口函数
void http_conn::process()
{
	HTTP_CODE read_ret = process_read();
	//NO_REQUEST，表示请求不完整，需要继续接收请求数据
	if(read_ret == NO_REQUEST)
	{
		modfd(m_epollfd, m_sockfd, EPOLLIN);
		return;
	}
	//调用process_write完成报文响应
	bool write_ret = process_write(read_ret);
	if(!write_ret)
	{
		close_conn();
	}
	//注册并监听写事件
	modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
