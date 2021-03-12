#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>

using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if(m_fp != NULL)
    {
        fclose(m_fp);
    }
}

// 异步设置阻塞队列长度，同步不设置
bool Log::init(const char *file_name, int log_buf_size, int split_lines, int max_queue_size)
{
    // 如果设置了max_queue_size,则设置为异步
    if(max_queue_size >= 1)
    {
        // 设置写入方式，true表示异步
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;

        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    // 输出内容的长度
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    // 日志的最大行数
    m_split_lines = split_lines;

    time_t t = time(NULL);
    // 把t分解为tm结构的时间结果
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    // char *strrchr(const char *str, int c) 在参数 str 所指向的字符串中搜索最后一次出现字符 c（一个无符号字符）的位置。
    // 返回的p是一个指向最后一个'/'字符的指针
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};
    if(p == NULL)
    {
        // p == NULL表示输入的file_name为空，则使用默认file_name,将时间+文件名作为日志名
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        // 将/的位置向后移动一个位置，然后复制到logname中
        strcpy(log_name, p+1);

        // p - file_name + 1是文件所在路径文件夹的长度,dirname相当于./即当前路径
        // 例如 "D:\Foam_git\jbFoamvarZ".
        // p指向jbFoamvarZ前面那个'\'，file_name指向初始位置D，因此p - file_name +1就是 "D:\Foam_git\"即dir_name
        strncpy(dir_name, file_name, p - file_name + 1);

        // 后面的参数跟format有关
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");
    if(m_fp == NULL)
    {
        return false;
    }

    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    // 第二个参数是时区
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    char s[16] = {0};
    // 日志分级
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
    case 3:
        strcpy(s, "[erro]:");
    default:
        strcpy(s, "[info]:");
        break;
    }

    m_mutex.lock();
    // 写入一个日志，计数加一
    m_count++;

    // 日志不是今天或写入的日志行数是最大行的倍数
    // m_split_lines是最大行数
    if(m_today != my_tm.tm_mday || m_count % m_split_lines == 0)
    {
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};

        // 格式化日志名中的时间部分
        snprintf(tail, 16, "%d_%02d_%02d", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        // 如果时间不是今天,则创建今天的日志(更新m_today)，更新m_today和m_count
        if(m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            // 超过了最大行，在之前的日志名基础上加后缀, m_count/m_split_lines
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count % m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }

    m_mutex.unlock();

    
    /*
    <Step 1> 在调用参数表之前，定义一个 va_list 类型的变量，(假设va_list valst)；
    <Step 2> 然后应该对valst 进行初始化，让它指向可变参数表里面的第一个参数，这是通过 va_start 来实现的，
    第一个参数是 valst 本身，第二个参数是在变参表前面紧挨着的一个变量,即“...”之前的那个参数；
    <Step 3> 然后是获取参数，调用va_arg，它的第一个参数是valst，第二个参数是要获取的参数的指定类型，
    然后返回这个指定类型的值，并且把 valst 的位置指向变参表的下一个变量位置；
    <Step 4> 获取所有的参数之后，我们有必要将这个 valst 指针关掉，以免发生危险，方法是调用 va_end，他是输入的参数 valst 置为 NULL
    ，应该养成获取完参数表之后关闭指针的习惯。说白了，就是让我们的程序具有健壮性。通常va_start和va_end是成对出现
    */
    // va_list 是一个字符指针，可以理解为指向当前参数的一个指针，取参必须通过这个指针进行。
    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入内容格式：时间 + 内容
    //时间格式化，snprintf成功返回写字符的总数，其中不包括结尾的null字符
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                    my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                    my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);

    // 把level以外的参数写入m_buf + n起始位置，返回写入到字符数组str中的字符个数(不包含终止符)
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    // n+m是已经写入的东西
    m_buf[n+m] = '\n';
    m_buf[n+m] = '\0';

    log_str = m_buf;

    m_mutex.unlock();

    if(m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        // 对m_fp写入需要加锁
        m_mutex.lock();
        // 同步模式或者阻塞队列满了直接把数据写入文件
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    fflush(m_fp);
    m_mutex.unlock();
}