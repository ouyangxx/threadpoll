#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WIN32
#include <windows.h>
#include <winsock2.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#endif
#include "threadpoll.h"

#define TIMER_DEFAULT           (10 * 1000) /*microseconds*/

#ifdef WIN32
static int gettimeofday(struct timeval *tp, void *tzp)
{
	time_t clock;
	struct tm tm;
	SYSTEMTIME wtm;
	GetLocalTime(&wtm);
	tm.tm_year = wtm.wYear - 1900;
	tm.tm_mon = wtm.wMonth - 1;
	tm.tm_mday = wtm.wDay;
	tm.tm_hour = wtm.wHour;
	tm.tm_min = wtm.wMinute;
	tm.tm_sec = wtm.wSecond;
	tm.tm_isdst = -1;
	clock = mktime(&tm);
	#ifdef WIN32
	tp->tv_sec = clock & 0xffffffff;//time_t/__int64(64bit) -> long(32bit)
	#else
	tp->tv_sec = clock;
	#endif
	tp->tv_usec = wtm.wMilliseconds * 1000;
	return (0);
}
#endif

typedef struct
{
    TASK_NAME task_name;
    void *task_param;
}threadpoll_task_t;

typedef struct
{
    int poll_state; //1,enable 0,disable

    /*describe the number of work thread*/
    int min_thread_num;
    int max_thread_num;
    int live_thread_num;
    int busy_thread_num;
    int wait_exit_thread_num;

    /*describe the task queue*/
    int max_task_queue_size;
    int task_queue_size;
    int task_queue_front;
    int task_queue_rear;
	threadpoll_task_t * task_queue;
    pthread_cond_t task_queue_not_full;
    pthread_cond_t task_queue_not_empty;

    pthread_t * work_tids;//set all work thread detached
    pthread_attr_t thr_attr;//detached

    pthread_t adjust_tid;//set adjust thread joinable

    pthread_mutex_t poll_lock;
	pthread_cond_t poll_not_enable;
	pthread_cond_t alive_thread_empty;

}threadpoll_t;

static void threadpoll_free(threadpoll_t * poll)
{
    if (poll != NULL)
    {
        if (poll->task_queue != NULL)
        {
            free(poll->task_queue);
            poll->task_queue = NULL;
        }
        if (poll->work_tids != NULL)
        {
            free(poll->work_tids);
            poll->work_tids = NULL;
        }
        pthread_cond_destroy(&poll->task_queue_not_full);
        pthread_cond_destroy(&poll->task_queue_not_empty);
        pthread_attr_destroy(&poll->thr_attr);
        pthread_mutex_destroy(&poll->poll_lock);
		pthread_cond_destroy(&poll->poll_not_enable);
		pthread_cond_destroy(&poll->alive_thread_empty);
        free(poll);
        poll = NULL;
    }
}

//1,enable 0,disable
static int threadpoll_enable(threadpoll_t * poll)
{
    if (NULL == poll)
    {
        return 0;
    }
    pthread_mutex_lock(&poll->poll_lock);
    int enable = poll->poll_state;
    pthread_mutex_unlock(&poll->poll_lock);
    return enable;
}

//1,alive 0,dead
static int threadpoll_alive(pthread_t tid)
{
    int res = pthread_kill(tid, 0);//test a thread is alive
    if (ESRCH == res)
    {
        return 0;
    }
    return 1;
}

void * worker_thread(void *arg)
{
    //printf("[threadpoll] [worker_thread] start.\n");

    threadpoll_t * poll = (threadpoll_t *)arg;
	threadpoll_task_t * task = NULL;
    while (1)
    {
        task = NULL;
        pthread_mutex_lock(&poll->poll_lock);

        while (poll->poll_state == 1 && poll->task_queue_size == 0) //task queue is empty
        {
            //printf("[threadpoll] [worker_thread] waiting....\n");
            pthread_cond_wait(&poll->task_queue_not_empty, &poll->poll_lock);

            //wait_exit_thread_num must be 0 before any work thread get task
            if (poll->wait_exit_thread_num > 0)
            {
                poll->wait_exit_thread_num -= 1;
                if (poll->live_thread_num > poll->min_thread_num)
                {
                    poll->live_thread_num -= 1;
					if (poll->live_thread_num <= 0)
					{
						pthread_cond_signal(&poll->alive_thread_empty);
					}
                    pthread_mutex_unlock(&poll->poll_lock);
                    //printf("[threadpoll] [worker_thread] exit.\n");
                    pthread_exit(NULL);
                }
            }
        }
        if (poll->poll_state == 0)
        {
            poll->live_thread_num -= 1;
			if (poll->live_thread_num <= 0)
			{
				pthread_cond_signal(&poll->alive_thread_empty);
			}
            pthread_mutex_unlock(&poll->poll_lock);
            //printf("[threadpoll] [worker_thread] exit.\n");
            pthread_exit(NULL);
        }

        //get the front task from task queue
        poll->task_queue_front = (poll->task_queue_front + 1) % poll->max_task_queue_size;
        task = &poll->task_queue[poll->task_queue_front];
        poll->task_queue_size -= 1;
        poll->busy_thread_num += 1;
        pthread_cond_signal(&poll->task_queue_not_full);

        pthread_mutex_unlock(&poll->poll_lock);

        //printf("[threadpoll] [worker_thread] get a task, working...\n");
        task->task_name(task->task_param);
        //printf("[threadpoll] [worker_thread] done a task.\n");

        pthread_mutex_lock(&poll->poll_lock);
        poll->busy_thread_num -= 1;
        pthread_mutex_unlock(&poll->poll_lock);
    }

    return NULL;
}

void * manager_thread(void *arg)
{
    //printf("[threadpoll] [manager_thread] start.\n");

    threadpoll_t * poll = (threadpoll_t *)arg;
    while (1)
    {
        pthread_mutex_lock(&poll->poll_lock);

        if (poll->poll_state == 0)
        {
            pthread_mutex_unlock(&poll->poll_lock);
            break;
        }

        if (poll->busy_thread_num == poll->live_thread_num && poll->task_queue_size > 0 && poll->live_thread_num < poll->max_thread_num)
        {
            int add = 0, i = 0;
            for (; i < poll->max_thread_num && poll->live_thread_num < poll->max_thread_num && add < poll->task_queue_size; ++i)
            {
				if (/*poll->work_tids[i] == 0 || */threadpoll_alive(poll->work_tids[i]) == 0)
                {
					pthread_create(&poll->work_tids[i], &poll->thr_attr, worker_thread, (void *)poll);
                    poll->live_thread_num += 1;
                    add += 1;
                }
            }
            if (add > 0)
            {
				//printf("[threadpoll] [manager_thread] task is too more, add %d worker threads\n", add);
            }
        }

        if (poll->busy_thread_num < poll->live_thread_num && (poll->live_thread_num - poll->busy_thread_num) > poll->task_queue_size && poll->live_thread_num > poll->min_thread_num)
        {
            poll->wait_exit_thread_num = poll->live_thread_num - poll->busy_thread_num - poll->task_queue_size;
            int i = 0;
            for (; i < poll->wait_exit_thread_num ; ++i)
            {
                pthread_cond_signal(&poll->task_queue_not_empty);
            }
        }

		//printf("[threadpoll] [manager_thread] min_thread_num:%d max_thread_num:%d live_thread_num:%d busy_thread_num:%d wait_exit_thread_num:%d task_queue_size:%d\n", poll->min_thread_num, poll->max_thread_num, poll->live_thread_num, poll->busy_thread_num, poll->wait_exit_thread_num, poll->task_queue_size);

		struct timeval now;
		gettimeofday(&now, NULL);
		int timeout_ms = TIMER_DEFAULT;
		long long nsec = now.tv_usec * 1000 + (timeout_ms % 1000) * 1000 * 1000;
		struct timespec abstime;
		abstime.tv_nsec = nsec % (1000 * 1000 * 1000);
		abstime.tv_sec = now.tv_sec + timeout_ms / 1000 + nsec / (1000 * 1000 * 1000);
		pthread_cond_timedwait(&poll->poll_not_enable, &poll->poll_lock, &abstime);
		if (poll->poll_state == 0)
		{
			pthread_mutex_unlock(&poll->poll_lock);
			break;
		}
        pthread_mutex_unlock(&poll->poll_lock);
    }

	pthread_mutex_lock(&poll->poll_lock);
	if (poll->live_thread_num > 0)
	{
		pthread_cond_wait(&poll->alive_thread_empty, &poll->poll_lock);
	}
	pthread_mutex_unlock(&poll->poll_lock);

    //printf("[threadpoll] [manager_thread] exit.\n");
    pthread_exit(NULL);
    return NULL;
}

THREAD_POLL threadpoll_create(int min_thread_num, int max_thread_num, int max_task_queue_size)
{
	//printf("[threadpoll] [threadpoll_create] min_thread_num:%d max_thread_num:%d max_task_queue_size:%d\n", min_thread_num, max_thread_num, max_task_queue_size);
    threadpoll_t * poll = (threadpoll_t *)malloc(sizeof(threadpoll_t));
    if (NULL == poll)
    {
        //malloc error
        return NULL;
    }

    poll->poll_state = 1; //enable

    poll->min_thread_num = min_thread_num;
    poll->max_thread_num = max_thread_num;
    poll->live_thread_num = 0;
    poll->busy_thread_num = 0;
    poll->wait_exit_thread_num = 0;

    poll->max_task_queue_size = max_task_queue_size;
    poll->task_queue_size = 0;
    poll->task_queue_front = 0;
    poll->task_queue_rear = 0;
	poll->task_queue = (threadpoll_task_t *)malloc(sizeof(threadpoll_task_t)* max_task_queue_size);
    if (NULL == poll->task_queue)
    {
        //malloc error
        threadpoll_free(poll);
        return NULL;
    }
    if (pthread_cond_init(&poll->task_queue_not_full, NULL) != 0)
    {
        //init cond error
        threadpoll_free(poll);
        return NULL;
    }
    if (pthread_cond_init(&poll->task_queue_not_empty, NULL) != 0)
    {
        //init cond error
        threadpoll_free(poll);
        return NULL;
    }
    poll->work_tids = (pthread_t *)malloc(sizeof(pthread_t) * max_thread_num);
    if (NULL == poll->work_tids)
    {
        //malloc error
        threadpoll_free(poll);
        return NULL;
    }
    memset(poll->work_tids, 0, sizeof(pthread_t) * max_thread_num); //work_tids[i] = 0 means not used now
    if (pthread_attr_init(&poll->thr_attr) != 0)
    {
        //init thread attribute error
        threadpoll_free(poll);
        return NULL;
    }
    if (pthread_attr_setdetachstate(&poll->thr_attr, PTHREAD_CREATE_DETACHED) != 0)
    {
        //set thread attribute error
        threadpoll_free(poll);
        return NULL;
    }
    if (pthread_mutex_init(&poll->poll_lock, NULL) != 0)
    {
        //init mutex error
        threadpoll_free(poll);
        return NULL;
    }
	if (pthread_cond_init(&poll->poll_not_enable, NULL) != 0)
	{
		//init cond error
		threadpoll_free(poll);
		return NULL;
	}
	if (pthread_cond_init(&poll->alive_thread_empty, NULL) != 0)
	{
		//init cond error
		threadpoll_free(poll);
		return NULL;
	}

    //start work thread, set detached
    int i = 0;
    for (; i < min_thread_num; ++i)
    {
        if (pthread_create(&poll->work_tids[i], &poll->thr_attr, worker_thread, (void *)poll) != 0)
        {
            //create thread error
			threadpoll_destroy((THREAD_POLL)poll);
            return NULL;
        }
        poll->live_thread_num += 1;
    }

    //start adjust thread, set joinable
	if (pthread_create(&poll->adjust_tid, NULL, manager_thread, (void *)poll) != 0)
    {
        //create thread error
		threadpoll_destroy((THREAD_POLL)poll);
        return NULL;
    }

	return (THREAD_POLL)poll;
}

static int set_max_thread_num(THREAD_POLL thr_poll, int max_thread_num)
{
    if (NULL == thr_poll)
    {
        return -1;
    }
    threadpoll_t * poll = (threadpoll_t *)thr_poll;
    pthread_mutex_lock(&poll->poll_lock);
    if (poll->max_thread_num >= max_thread_num)
    {
        poll->max_thread_num = max_thread_num;
    }
    else
    {
        pthread_t *work_tids = (pthread_t *)malloc(sizeof(pthread_t) * max_thread_num);
        if (NULL == work_tids)
        {
            //malloc error
            pthread_mutex_unlock(&poll->poll_lock);
            return -1;
        }
        memset(work_tids, 0, sizeof(pthread_t) * max_thread_num); //work_tids[i] = 0 means not used now
        if (poll->work_tids != NULL)
        {
            int i = 0;
            for (; i < poll->max_thread_num; ++i)
            {
                work_tids[i] = poll->work_tids[i];
            }
            free(poll->work_tids);
            poll->work_tids = NULL;
        }
        poll->work_tids = work_tids;
        poll->max_thread_num = max_thread_num;
    }
    pthread_mutex_unlock(&poll->poll_lock);

    return 0;
}

int threadpoll_add_task(THREAD_POLL thr_poll, TASK_NAME task_name, void *task_param)
{
    if (NULL == thr_poll || NULL == task_name)
    {
        return -1;
    }
    threadpoll_t * poll = (threadpoll_t *)thr_poll;
    pthread_mutex_lock(&poll->poll_lock);

    while (poll->poll_state == 1 && poll->task_queue_size == poll->max_task_queue_size)
    {
        //task queue is full, waiting for task_queue_not_full
        pthread_cond_wait(&poll->task_queue_not_full, &poll->poll_lock);
    }
    if (poll->poll_state == 0)
    {
        //poll is disable
        pthread_mutex_unlock(&poll->poll_lock);
        return -1;
    }

    //append a new task to the task queue
    poll->task_queue_rear = (poll->task_queue_rear + 1) % poll->max_task_queue_size;
    poll->task_queue[poll->task_queue_rear].task_name = task_name;
    poll->task_queue[poll->task_queue_rear].task_param = task_param;
    poll->task_queue_size += 1;

    //wake up a thread that is waiting for task_queue_not_empty
    pthread_cond_signal(&poll->task_queue_not_empty);

    pthread_mutex_unlock(&poll->poll_lock);
    return 0;
}

int threadpoll_destroy(THREAD_POLL thr_poll)
{
    if (NULL == thr_poll)
    {
        return -1;
    }
    threadpoll_t *poll = (threadpoll_t *)thr_poll;
    pthread_mutex_lock(&poll->poll_lock);
    poll->poll_state = 0; //disable the thread poll
    pthread_cond_broadcast(&poll->task_queue_not_empty); //wake up all thread that is waiting for task_queue_not_empty
    pthread_cond_broadcast(&poll->task_queue_not_full);  //wake up all thread that is waiting for task_queue_not_full
	pthread_cond_signal(&poll->poll_not_enable);
    pthread_t adjust_tid = poll->adjust_tid;
    pthread_mutex_unlock(&poll->poll_lock);

    pthread_join(adjust_tid, NULL);
    threadpoll_free(poll);
    poll = NULL;

	//printf("[threadpoll] [threadpoll_destroy] destroy.\n");
    return 0;
}
