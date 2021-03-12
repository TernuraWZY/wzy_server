#include "lst_timer.h"

sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while(tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}
// 添加定时器，内部调用私有成员add_timer
void sort_timer_lst::add_timer(util_timer *timer)
{
    if(!timer)
    {
        return;
    }
    // head为空则表示定时器链表无元素，则此时直接head = tail = timer
    if(!head)
    {
        head = tail = timer;
        return;
    }
    // timer的延迟时间最短时放到最前
    if(timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}
// 调整定时器，任务发生变化时，调整定时器在链表中的位置,一般是加时
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if(!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    // 被调整的定时器在链表尾部
    // 定时器超时值仍然小于下一个定时器超时值，不调整
    if(!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    // 被调整定时器是链表头结点，将定时器取出，重新插入
    if(timer == head)
    {
        head = head->next;
        head->prev = nullptr;
        timer->next = nullptr;
        add_timer(timer, head);
    }
    // 被调整定时器在内部，将定时器取出，重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

void sort_timer_lst::del_timer(util_timer *timer)
{
    if(!timer)
    {
        return;
    }

    // 链表中只有一个定时器，需要删除该定时器
    if((timer == head) && (timer == tail))
    {
        delete timer;
        head = nullptr;
        tail = nullptr;
        return;
    }

    // 被删除的定时器为头结点
    if(head == timer)
    {
        head = head->next;
        head->prev = nullptr;
        delete timer;
        return;
    }

    // 被删除的定时器为尾结点
    if(tail == timer)
    {
        tail = tail->prev;
        tail->next = nullptr;
        delete timer;
        return;
    }

    // 被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
// 定时任务处理函数
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }  
    LOG_INFO("%s\n", "timer tick");
    Log::get_instance()->flush();
    // 获取当前时间
    time_t cur = time(NULL);
    util_timer *tmp = head;
    while(tmp)
    {
        // 链表容器为升序排列
        // 当前时间小于定时器的超时时间，后面的定时器也没有到期
        if(cur < tmp->expire)
        {
            break;
        }

        // 当前定时器到期，则调用回调函数，执行定时事件(删除事件)
        tmp->cb_func(tmp->user_data);

        // 将处理后的定时器从链表容器中删除，并重置头结点
        head = tmp->next;
        if(head)
        {
            head->prev = nullptr;
        }
        delete tmp;
        tmp = head;
    }
}

// 主要用于调整链表内部结点
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;

    // 遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置，常规双向链表插入操作,注意如果调用add_timer，则表明timer并非位于头部，而是需要内部插入
    while(tmp)
    {
        if(timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            timer->prev = prev;
            tmp->prev = timer;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    // 表示到达末尾了
    if(!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = nullptr;
        tail = timer;
    }
}