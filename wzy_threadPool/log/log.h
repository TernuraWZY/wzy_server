#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

class Log
{
public:
    // C++11新标准，使用饿汉模型不加锁
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }

    // C++11之前写法如下
    // static Log *get_instance()
    // {
    //     pthread_mutex_lock(&lock);
    //     static Log instance;
    //     pthread_mutex_unlock(&lock);
    //     return &instance;
    // }

    // 第二种写法，不用局部静态变量
    // static Log* p;
    // static Log *get_instance()
    // {
    // 如果只检测一次，在每次调用获取实例的方法时，都需要加锁，这将严重影响程序性能。
    // 双层检测可以有效避免这种情况，仅在第一次创建单例的时候加锁，其他时候都不再符合NULL == p的情况，直接返回已创建好的实例。
    //     if(p == NULL)
    //     {
    //         pthread_mutex_lock(&lock);
    //         if(p == NULL)
    //         {
    //             p = new Log;
    //         }
    //         pthread_mutex_unlock(&lock);
    //     }
    //     return p;
    // }

    // 异步写日志公有方法，调用私有方法async_write_log,用void*为了和pthread_create匹配
    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }
    // 可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);
    // 将输出内容按照标准格式整理，这里相当于把字符串放入阻塞队列，是生产者
    void write_log(int level, const char *format, ...);
    // 强制刷新
    void flush(void);
private:
    // 单例模式构造函数私有
    Log();

    virtual ~Log();

    // 异步写日志,这个是作为线程创建的函数指针参数传入，即工作线程相当于一直在运行这个向文件写日志函数，取出阻塞队列的字符串进行写，是消费者
    void *async_write_log()
    {
        string single_log;
        // 从阻塞队列去除一个日志string，写入文件
        while(m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            // s 代表要输出的字符串的首地址，可以是字符数组名或字符指针变量名。
            // stream 表示向何种流中输出，可以是标准输出流 stdout，也可以是文件流。标准输出流即屏幕输出，printf 其实也是向标准输出流中输出的。
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

private:
    char dir_name[128];     //路径名
    char log_name[128];     //log文件名
    int m_split_lines;      //日志最大行数
    int m_log_buf_size;     //日志缓冲区大小
    long long m_count;      //日志当前行数
    int m_today;            //按天分类，记录当前时间是哪一天 
    FILE *m_fp;             // 打开log文件的指针
    char *m_buf;
    block_queue<string> *m_log_queue;   //阻塞队列
    bool m_is_async;                    //是否同步标志位
    locker m_mutex;
};

#define LOG_DEBUG(format, ...) Log::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) Log::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Log::get_instance()->write_log(3, format, ##__VA_ARGS__)

#endif