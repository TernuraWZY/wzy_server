#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include "sql_connection_pool.h"

using namespace std;

// RAII机制销毁连接池
connection_pool::~connection_pool()
{
    destroyPool();
}

connection_pool *connection_pool::getInstance()
{
    // 返回局部静态对象的饿汉模式
    static connection_pool connPool;
    return &connPool;
}

void connection_pool::init(string url, string user, string passWord, string dataBaseName, int port, unsigned int maxConn)
{
    this->m_url = url;
    this->m_user = user;
    this->m_passWord = passWord;
    this->m_dataBaseName = dataBaseName;
    this->m_port = port;
    this->m_maxConn = maxConn;

    lock.lock();
    // 创建maxConn条数据库连接
    for(int i = 0; i < m_maxConn; ++i)
    {
        MYSQL *con = NULL;
        con = mysql_init(con);

        if(con == NULL)
        {
            cout << "error : " << mysql_error(con);
            exit(1);
        }
        /*
        MYSQL *mysql_real_connect (
        MYSQL *mysql,
        const char *host,
        const char *user, 
        const char *passwd, 
        const char *db, 
        unsigned int port,
        const char *unix_socket,
        unsigned long client_flag)
        上面描述了五个参数的主要取值，
        MYSQL *为mysql_init函数返回的指针，
        host为null或 localhost时链接的是本地的计算机，
        当mysql默认安装在unix（或类unix）系统中，root账户是没有密码的，因此用户名使用root，密码为null，
        当db为空的时候，函数链接到默认数据库，在进行 mysql安装时会存在默认的test数据库，因此此处可以使用test数据库名称，
        port端口为0，
        使用 unix连接方式，unix_socket为null时，表明不使用socket或管道机制，最后一个参数经常设置为0
        mysql_real_connect()尝试与运行在主机上的MySQL数据库引擎建立连接。在你能够执行需要有效MySQL连接句柄结构的任何其他API函数之前，
        mysql_real_connect()必须成功完成。
        如果连接成功，返回MYSQL*连接句柄。如果连接失败，返回NULL。对于成功的连接，返回值与第1个参数的值相同。
        */
        con = mysql_real_connect(con, url.c_str(), user.c_str(), passWord.c_str(), dataBaseName.c_str(), port, NULL, 0);
        if(con == NULL)
        {
            cout << "error : " << mysql_error(con);
            exit(1);
        }        
        connList.push_back(con);
        ++m_freeConn;
    }
        // 初始化信号量值为m_freeConn
        reserve = sem(m_freeConn);
        this->m_maxConn = m_freeConn;

        lock.unlock();
}

// 当有请求时，从数据库连接池返回一个可用连接，并且更新使用和空闲连接数
MYSQL *connection_pool::getConnection()
{
    MYSQL *con = nullptr;

    if(connList.size() == 0)
    {
        return nullptr;
    }
    // 如果信号量值不为0，wait直接返回，否则阻塞
    reserve.wait();

    lock.lock();

    con = connList.front();
    connList.pop_front();

    --m_freeConn;
    ++m_curConn;

    lock.unlock();
    return con;
}

// 释放当前使用的连接
bool connection_pool::releaseConnection(MYSQL *conn)
{
    if(conn == nullptr)
    {
        return false;
    }

    lock.lock();

    connList.push_back(conn);
    ++m_freeConn;
    --m_curConn;

    lock.unlock();

    reserve.post();
    return true;
}

// 销毁数据库连接池
void connection_pool::destroyPool()
{
    lock.lock();
    if(connList.size() > 0)
    {
        for(auto it = connList.begin(); it != connList.end(); ++it)
        {
            MYSQL *con = *it;
            mysql_close(con);
        }
        m_curConn = 0;
        m_freeConn = 0;
        connList.clear();
    }
    lock.unlock();
}

// 当前空闲的连接数
int connection_pool::getFreeConn()
{
    return this->m_freeConn;
}

// 连接池RAII类构造
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
    *SQL = connPool->getConnection();

    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->releaseConnection(conRAII);
}