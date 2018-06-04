/**
 * 在应用程序启动之后，就马上创建一定数量的线程，放入空闲的线程池中。
 * 这些线程都是处于阻塞状态，这些线程只占一点内存，不占用CPU。
 * 当任务到来后，线程池将选择一个空闲的线程，将任务传入此线程中运行。
 * 
 * 当所有的线程都处在处理任务的时候，线程池将自动创建一定数量的新线程，用于处理更多的任务；算法：queue_size >= busy_thr_num
 * 执行任务完成之后线程并不退出，而是继续在线程池中等待下一次任务；
 * 当大部分线程处于阻塞状态时，线程池将自动销毁一部分的线程，回收系统资源。算法：live_thr_num > 2*busy_thr_num
 */

#include "threadpoll.h"
#include <stdio.h>
#include <stdlib.h>

void *process(void *arg)
{
	printf("thread 0x%x is on job, arg=%d\n", (unsigned int)pthread_self(), *(int *)arg);
	sleep(1);
	return NULL;
} 

int main(void)
{
	/*线程池里最少要有3个线程存活，最多存活40个，循环队列数组长度为20*/
	threadpoll_t *thp = threadpoll_create(3, 40, 20);

	printf("\npoll inited...\n");

	int *num = (int *)malloc(sizeof(int) * 50); /*创建一个数组*/
	int i;
	for (i=0; i<50; i++) {
		num[i] = i;
		printf("add task %d\n", i);
		threadpoll_add(thp, process, (void *)&num[i]);/*添加50个任务到线程池的任务队列中，但队列只能放30个任务，因此会有阻塞*/
	}
	sleep(10);
	threadpoll_destroy(thp);
	free(num);

	return 0;
}
