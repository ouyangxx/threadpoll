#ifndef __THREADPOLL_H__
#define __THREADPOLL_H__

#ifdef __cplusplus
extern "C"
{
#endif

typedef void * THREAD_POLL;

typedef void * (*TASK_NAME)(void *);

THREAD_POLL threadpoll_create(int min_thread_num, int max_thread_num, int max_task_queue_size);

int threadpoll_add_task(THREAD_POLL poll, TASK_NAME task_name, void *task_param);

int threadpoll_destroy(THREAD_POLL poll);

#ifdef __cplusplus
}
#endif

#endif
