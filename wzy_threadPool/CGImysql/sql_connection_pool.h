#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"

using namespace std;

class connection_pool
{
public:
    connection_pool() : m_curConn(0), m_freeConn(0) {};
    ~connection_pool();

    // 获取数据库连接
    MYSQL *getConnection();
    // 释放连接
    bool releaseConnection(MYSQL *conn);

    int getFreeConn();

    void destroyPool();

    // 单例模式
    static connection_pool *getInstance();

    void init(string url, string user, string passWord, string dataBaseName, int port, unsigned int maxConn);

private:
    // 最大连接数
    unsigned int m_maxConn;
    // 当前已经使用的连接数
    unsigned int m_curConn;
    // 当前空闲连接数
    unsigned int m_freeConn;

private:
    locker lock;
    // 连接池
    list<MYSQL *> connList;

    sem reserve;
private:
    // 主机地址
    string m_url;
    // 数据库端口号
    string m_port;
    // 登录数据库用户名
    string m_user;
    // 登录数据库密码
    string m_passWord;
    // 使用数据库名
    string m_dataBaseName;
};

// 管理数据库资源类
class connectionRAII
{
public:
    connectionRAII(MYSQL **conn, connection_pool *connPool);
    ~connectionRAII();

private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};

#endif