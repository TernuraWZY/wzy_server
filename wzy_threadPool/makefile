# server: 
# 	server.cpp ./threadpool/threadpool.h ./http/http_conn.cpp ./http/http_conn.h \
# 	./lock/locker.h \
# 	./log/log.cpp ./log/log.h \
# 	./log/block_queue.cpp ./log/block_queue.h
	rm  -r server
	rm *ServerLog
# 	g++ -w -o server \
# 	./threadpool/threadpool.h \
# 	./http/http_conn.cpp ./http/http_conn.h \
# 	./timer/lst_timer.h ./timer/lst_timer.cpp \
# 	./lock/locker.h \
# 	./log/block_queue.cpp ./log/block_queue.h \
# 	./log/log.cpp ./log/log.h \
# 	server.cpp -lpthread 

g++ -w -o server server.cpp log/log.cpp timer/lst_timer.cpp http/http_conn.cpp \
CGImysql/sql_connection_pool.cpp log/block_queue.h threadpool/threadpool.h lock/locker.h \
-lpthread -lmysqlclient


# clean:
# 	rm  -r server
