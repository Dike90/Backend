#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

using namespace std;
//线程池类，将它定义为模板是为了代码复用，模板参数T是任务类
template<typename T>
class threadpool
{
public:
    //参数thread_number是线程池中线程的数量，max_requests表示请求队列中最多允许的等待请求
    threadpool( int thread_number = 8 , int max_requests = 10000);
    ~threadpool();
    //向请求队列中添加任务
    bool append(T* request);

private:
    //工作线程运行的函数，它不断从工作队列中取出任务并执行
    static void* worker(void *arg);
    void run();

private:
    //线程池中线程数
    int m_thread_number;
    //请求队列中允许的最大请求数量
    int m_max_requests ;
    //描述线程池的数组指针，数组大小为m_thread_number
    pthread_t* m_threads;
    //请求队列
    std::list<T*> m_workqueue;
    //保护请求队列的互斥锁
    locker m_queuelocker;
    //是否有任务需要处理
    sem m_queuestat ;
    //是否结束线程
    bool m_stop;
};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL)
{
    if (( thread_number <= 0) || (max_requests <=0))
        throw std::exception();
    //创建线程ID数组
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    //创建线程池，当遇到错误会抛出异常,线程函数为worker，并将该对象的指针传递给线程函数
    for ( int i = 0; i< thread_number ; i++){
        cout <<"create the "<<i <<"th thread"<<endl;
        if ( pthread_create(m_threads+i, NULL , worker , this) != 0 ){
            delete [] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])){
            delete [] m_threads;
            throw std::exception();
        }

    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete [] m_threads;
    m_stop = true;
}

//向请求队列中添加任务
template<typename T>
bool threadpool<T>::append(T* request)
{
    //线程池共享请求队列，所以在操作请求队列的时候需要加锁
    m_queuelocker.lock();
    if ( m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    //将任务添加到m_workqueue的链表中
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

//线程函数，运行了run函数
template<typename T>
void* threadpool<T>::worker(void* arg)
{
    threadpool *pool = (threadpool*)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    while(!m_stop)
    {
        //等待信号量，如果当前信号量为0，则所有的线程都将被阻塞
        //如果当前信号量不为0，则某个线程将会执行sem_wait()操作，将这个信号量减一
        m_queuestat.wait();
        //首先尝试获取任务队列的锁
        m_queuelocker.lock();
        //获取成功就检查任务队列中是否存在任务，如果存在任务就将任务取出，取出任务后，将获取的锁解锁，将允许其他进程进行操作
        if (m_workqueue.empty()){
            //如果任务队列为空，就将获得的锁解锁，并退出当前循环，进行下一次循环
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        //任务合法，就处理该任务
        if (!request)
            continue;
        request->process();
    }
}

#endif // __THREADPOOL_H__
