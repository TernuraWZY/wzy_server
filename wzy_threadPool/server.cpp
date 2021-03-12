#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <malloc.h>
#include <sys/epoll.h>
#include <cassert>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <fcntl.h>
#include "lock/locker.h"
#include "threadpool/threadpool.h"
#include "log/log.h"
#include "http/http_conn.h"
#include "CGImysql/sql_connection_pool.h"
#include "timer/lst_timer.h"

#define MAX_FD 65536            // 最大文件描述符
#define MAX_EVENT_NUMBER 10000  // 最大事件数
#define TIMESLOT 100             // 最小超时单位

// #define SYNLOG                  // 同步写日志
#define ASYNLOG                 // 异步写日志

// 水平触发阻塞
//#define listenfdLT
// 边缘触发非阻塞
#define listenfdET

// 这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

// 信号处理函数
void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;    
}

// 设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true)
{
    // 创建sigaction结构体变量
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    // 信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    // 将所有信号添加到信号集中
    sigfillset(&sa.sa_mask);// 在sa_mask信号集中设置所有信号，意味着所有信号都被屏蔽了

    // 执行sigaction函数
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    // alarm函数定时发送SIGALARM信号给进程
    alarm(TIMESLOT);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭，当定时器时间到期时调用该函数对事件进行删除
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d\n", user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8);
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0);
#endif


    if(argc <= 2)
    {
        printf("usage: %s ip_address port number \n", basename(argv[0]));
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    doc_root = 
    // 忽略sigpipe信号
    addsig(SIGPIPE, SIG_IGN);

    // 创建数据库连接池(静态对象)
    connection_pool *connPool = connection_pool::getInstance();
    connPool->init("localhost", "root", "123456", "wzydb", 3306, 8);

    // 创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch(...)
    {
        return 1;
    }

    // 预先为每个可能的客户连接分配一个http_conn对象
    http_conn *users = new http_conn[MAX_FD];
    assert(users);
    int user_count = 0;
    // 注意我们申请了MAX_FD个http_conn，但是实际上initmysql_result只做了一次，故存放账号密码对应关系的map只初始化了一个，因为它是全局变量
    users->initmysql_result(connPool);
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd != -1);
    // close()立刻返回，但不会发送未发送完成的数据，而是通过一个REST包强制的关闭socket描述符，即强制退出。
    // struct linger tmp = {1, 0};
    // setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    ret = bind(listenfd, (struct sockaddr*) &address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);
    //将listenfd注册到epoll事件表上
    addfd(epollfd, listenfd, false);
    //将上述epollfd赋值给http类对象的m_epollfd属性
    http_conn::m_epollfd = epollfd;

    // 创建管道
    ret = socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    // pipefd[0]是读端
    addfd(epollfd, pipefd[0], false);

    // 当信号到来时，sig_handler起作用，想pipe[0]发送信号再进行处理，统一信号源
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(TIMESLOT);

    while(!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if(number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s\n", "epoll failure");
            break;
        }
        for(int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                int connfd = accept(listenfd, (struct sockaddr*) &client_address, &client_addrlength);
                if(connfd < 0)
                {
                    LOG_ERROR("%s:errno is : %d\n", "accept error", errno);
                    continue;
                }
                if(http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "too many users, server is busy");
                    LOG_ERROR("%s\n", "too many users, server is busy")
                    continue;
                }
                // 初始化客户连接
                users[connfd].init(connfd, client_address);

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur+3*TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdET
                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d\n", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "too many users, server is busy");
                        LOG_ERROR("%s\n", "too many users, server is busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // EPOLLRDHUP = 对端断开连接    EPOLLHUP EPOLLERR = 本端(服务端)出问题
                // 服务器关闭连接，移除对应的定时器            
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if(timer)
                {
                    timer_lst.del_timer(timer);
                }

                // 出现异常，立即关闭连接
                printf("connection error\n");
                // users[sockfd].close_conn();
            }
            // 处理信号
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1)
                {
                    continue;
                }
                else if(ret == 0)
                {
                    continue;
                }
                else
                {
                    for(int i = 0; i < ret; ++i)
                    {
                        switch(signals[i])
                        {
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            // 处理客户连接上接收到的数据
            else if(events[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer;
                // 根据读的结果，决定是将任务加到线程池还是关闭连接
                if(users[sockfd].read())
                {
                    LOG_INFO("deal with the client (%s)\n", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    // 若监测到读事件，将该事件放入请求队列
                    pool->append_job(users+sockfd);

                    // 若有数据传输，则将定时器往后延迟3个单位
                    // 并对新的定时器在链表上的位置进行调整
                    if(timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur+3*TIMESLOT;

                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if(events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                // 根据写的结果，决定是否关闭连接，注意write函数如果需要写return true
                if(users[sockfd].write())
                {
                    LOG_INFO("send data to the client (%s)\n", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if(timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur+3*TIMESLOT;
                        LOG_INFO("%s\n", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            if(timeout)
            {
                // timer_handler函数会设置心搏函数alarm(TIMESLOT),因此会周期性触发SIGALARM信号，触发信号后timeout = true形成了周期监测事件的闭环
                timer_handler();
                timeout = false;
            }
            
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
