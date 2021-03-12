#ifndef LST_TIMER
#define LST_TIMER

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
#include <sys/wait.h>
#include <sys/uio.h>
#include "../log/log.h"
#include <time.h>

// 前置声明
class util_timer;
struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};
// 时间升序链表的节点
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    // 超时时间
    time_t expire;
    // 回调函数,当定时器到时后，用于关闭文件描述符等收尾工作
    void (*cb_func)(client_data*);
    // 连接资源
    client_data *user_data;
    // 前向定时器
    util_timer *prev;
    // 后向定时器
    util_timer *next;
};

class sort_timer_lst
{
public:
    sort_timer_lst() : head(NULL), tail(NULL) {};

    // 析构函数主要为了销毁timer链表
    ~sort_timer_lst();

    // 添加针对连接的定时器，内部调用add_timer
    void add_timer(util_timer *timer);

    // 调整定时器，任务发生变化时调整定时器在链表位置
    void adjust_timer(util_timer *timer);

    // 删除定时器
    void del_timer(util_timer *timer);

    // 定时任务处理函数
    void tick();
private:
    //私有成员，被公有成员add_timer和adjust_time调用
    //主要用于调整链表内部结点
    void add_timer(util_timer *timer, util_timer *lst_head);
    // 头尾节点
    util_timer *head;
    util_timer *tail;
};

#endif
