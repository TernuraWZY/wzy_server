1. 线程池初始化，创建8个工作线程，并且包含一个http*类型的请求队列
2. 当主线程接收到客户数据时，调用append_job往请求队列添加http*类型数据，即添加某个连接，并且调用post让信号量+1
3. 线程初始阶段阻塞在sem_wait上，当append_job后信号量大于一，线程sem_wait阻塞状态解除，从请求队列取出资源(http*)并运行(http*)request->process()