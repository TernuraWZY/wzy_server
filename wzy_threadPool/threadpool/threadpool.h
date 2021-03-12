#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
	//thread_number是线程池中线程数量，max_requests是请求队列中允许最多等待处理的请求
	threadpool(connection_pool *connPool, int thread_number = 8, int max_requests = 10000);
	~threadpool();
	//往请求队列添加任务
	bool append_job(T* request);

private:
	//工作线程运行的函数,它从工作队列取出任务并且执行
	static void* worker(void* arg);
	void run();

private:
	int m_thread_number;			//线程池允许的线程数
	int m_max_requests;				//请求队列中允许的最大请求数
	pthread_t* m_threads; 			//描述线程池的数组，大小为m_thread_number
	std::list<T* > m_workqueue; 	//请求队列
	locker m_queuelocker; 			//保护请求队列的锁(互斥锁)
	sem m_queuestat; 				//是否有任务要处理(信号量)
	bool m_stop; 					//是否结束线程
	connection_pool *m_connPool; 	//数据库连接池
};

template<typename T>
threadpool<T>::threadpool(connection_pool *connPool, int thread_number, int max_requests)	:
	m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL), m_connPool(connPool)
{
	if((thread_number <= 0) || (max_requests <= 0))
	{
		throw std::exception();
	}

	m_threads = new pthread_t[m_thread_number];
	if(!m_threads)
	{
		throw std::exception();
	}

	//创建thread_number个线程，并将它们设置为脱离线程
	for(int i = 0; i < thread_number; ++i)
	{
		//第一个参数为pthread_t*类型，用于标识一个创建的线程
		if(pthread_create(m_threads+i, NULL, worker, this) != 0)
		{
			delete [] m_threads;
			throw std::exception();
		}
		//这将该子线程的状态设置为detached,则该线程运行结束后会自动释放所有资源
		if(pthread_detach(m_threads[i]))
		{
			delete [] m_threads;
			throw std::exception();
		}
	}
}

template<typename T>
threadpool<T>::~threadpool()
{
	delete [] m_threads;
	m_stop = true;
}

//往请求队列添加任务
template<typename T>
bool threadpool<T>::append_job(T* request)
{
	//对工作队列的操作先加锁，因为它被所有线程共享
	m_queuelocker.lock();
	//已经超过了最大允许请求数目
	if(m_workqueue.size() > m_max_requests)
	{
		m_queuelocker.unlock();
		return false;
	}

	m_workqueue.push_back(request);
	m_queuelocker.unlock();
	//post给信号量P操作加一，这里目的是为了告诉主线程，请求队列中有数据了，可以接收
	m_queuestat.post();//
	return true;
}

//worker函数是传给pthread_create静态函数的参数，我们想要在静态函数中调用类动态成员函数，需要将类对象(this)作为参数
//传给pthread_create,通过this进行动态成员函数调用
template<typename T>
void* threadpool<T>::worker(void* arg)
{
	threadpool* pool = (threadpool*) arg;
	pool->run();//动态成员函数调用
	return pool;
}

template<typename T>
void threadpool<T>::run()
{
	while(!m_stop)
	{
		//该步操作为了等待append_job处的信号量的p操作实之大于0，表示请求队列有请求
		//如果信号量为0，wait调用就会阻塞
		m_queuestat.wait();
		m_queuelocker.lock();//对工作请求队列操作加锁

		if(m_workqueue.empty())
		{
			m_queuelocker.unlock();
			continue;
		}
		//取出请求队列任务
		T* request = m_workqueue.front();
		m_workqueue.pop_front();
		m_queuelocker.unlock();
		if(!request)
		{
			continue;
		}
		// 这里初始化了mysql连接，获取一个向mysql数据库的连接
		connectionRAII mysqlcon(&request->mysql, m_connPool);
		//调用T中的process方法
		request->process();
	}
}

#endif