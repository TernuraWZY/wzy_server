#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

//封装信号量的类
class sem
{
public:

	//创建并初始化信号量
	sem()
	{
		//sem_init成功返回0，不为0则抛出异常！！注意信号量描述符初始化和析构需要&
		if(sem_init(&m_sem, 0, 0) != 0)
		{
			throw std::exception();
		}
	}
	sem(int num)
	{
		// 第三个参数为信号量的值
		if(sem_init(&m_sem, 0, num) != 0)
		{
			throw std::exception();
		}
	}

	//析构函数，销毁信号量
	~sem()
	{
		sem_destroy(&m_sem);
	}

	//等待信号量，以原子性操作将信号量值减一，如果信号量值为0，将被阻塞，直到信号量具有非0值
	bool wait()
	{
		return sem_wait(&m_sem) == 0;
	}

	//增加信号量,当有sem_wait调用时，将会唤醒
	bool post()
	{
		return sem_post(&m_sem) == 0;
	}
private:
	sem_t m_sem;
};

//封装互斥锁类,用于同步线程对共享数据的访问
class locker
{
public:

	//构造函数
	locker()
	{
		if(pthread_mutex_init(&m_mutex, NULL) != 0)
		{
			throw std::exception();
		}
	}

	//析构函数
	~locker()
	{
		pthread_mutex_destroy(&m_mutex);
	}

	//获取互斥锁
	bool lock()
	{
		return pthread_mutex_lock(&m_mutex) == 0;
	}

	//释放锁
	bool unlock()
	{
		return pthread_mutex_unlock(&m_mutex) == 0;
	}

	pthread_mutex_t *get()
	{
		return &m_mutex;
	}
private:
	pthread_mutex_t m_mutex;
};

//条件变量，用于同步线程间共享数据的值
class cond
{
public:
	//创建初始化条件变量
	cond()
	{ 
		if(pthread_cond_init(&m_cond, NULL) != 0)
		{
			// pthread_mutex_destroy(&m_cond);
			throw std::exception();
		}
	}

	//销毁条件变量
	~cond()
	{
		pthread_cond_destroy(&m_cond);
	}

	//wait条件变量
	bool wait(pthread_mutex_t *m_mutex)
	{
		int ret = 0;
		//pthread_mutex_lock(&m_mutex);
		ret = pthread_cond_wait(&m_cond, m_mutex);
		//pthread_mutex_unlock(&m_mutex);
		return ret == 0;
	}

	bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
	{
		int ret = 0;

		ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);

		return ret == 0;
	}

	bool signal()
	{
		return pthread_cond_signal(&m_cond) == 0;
	}

    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
	pthread_cond_t m_cond;
};

#endif
