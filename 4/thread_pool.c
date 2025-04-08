#include "thread_pool.h"
#include <pthread.h>
#include <stdlib.h>

/** Текущее состояние задачи.
 *
 * Используется для отслеживания жизненного цикла задачи внутри пула.
 */
enum thread_task_state
{
	TASK_STATE_NEW,		 // задача только создана, ещё не добавлена в пул
	TASK_STATE_IN_POOL,	 // задача была передана в пул и стоит в очереди
	TASK_STATE_RUNNING,	 // задача сейчас выполняется рабочим потоком
	TASK_STATE_FINISHED, // задача завершила выполнение, результат доступен
	TASK_STATE_DETACHED, // задача была отсоединена и будет удалена автоматически
};

struct thread_task
{
	thread_task_f function; // Указатель на функцию задачи: void* f(void *arg)
	void *arg;				// Аргумент, передаваемый в функцию
	void *result;			// Результат выполнения функции (возвращается в join)

	pthread_mutex_t mutex; // Мьютекс для защиты полей задачи
	pthread_cond_t cond;   // Нужна, чтобы другие потоки могли ждать окончания задачи

	enum thread_task_state state; // Текущее состояние задачи

	struct thread_task *next; // Указатель на следующую задачу в очереди (односвязный список)
};

struct thread_pool
{
	pthread_t *threads;		 // Массив потоков
	int max_thread_count;	 // Максимальное количество потоков
	int active_thread_count; // Текущее количество созданных потоков

	struct thread_task *task_queue_head; // Начало очереди задач
	struct thread_task *task_queue_tail; // Конец очереди задач
	int active_task_count;				 // Сколько задач сейчас в очереди или выполняется

	pthread_mutex_t queue_mutex; // Мьютекс для синхронизации очереди задач
	pthread_cond_t queue_cond;	 // Для пробуждения рабочих потоков

	bool is_shutting_down; // Флаг — идёт удаление пула
};

int thread_pool_new(int max_thread_count, struct thread_pool **pool)
{
	if (max_thread_count < 1 || max_thread_count > TPOOL_MAX_THREADS)
		return TPOOL_ERR_INVALID_ARGUMENT;

	struct thread_pool *new_pool = malloc(sizeof(struct thread_pool));
	if (new_pool == NULL)
		return TPOOL_ERR_NO_MEMORY;

	new_pool->threads = calloc(max_thread_count, sizeof(pthread_t));
	if (new_pool->threads == NULL)
	{
		free(new_pool);
		return TPOOL_ERR_NO_MEMORY;
	}

	new_pool->max_thread_count = max_thread_count;
	new_pool->active_thread_count = 0;

	new_pool->task_queue_head = NULL;
	new_pool->task_queue_tail = NULL;
	new_pool->active_task_count = 0;

	pthread_mutex_init(&new_pool->queue_mutex, NULL);
	pthread_cond_init(&new_pool->queue_cond, NULL);

	new_pool->is_shutting_down = false;

	*pool = new_pool;

	return 0;
}

int thread_pool_thread_count(const struct thread_pool *pool)
{
	return pool->active_thread_count;
}

int thread_pool_delete(struct thread_pool *pool)
{
	pthread_mutex_lock(&pool->queue_mutex);

	// Если есть незавершенные задачи — удалить нельзя
	if (pool->task_queue_head != NULL || pool->active_task_count > 0)
	{
		pthread_mutex_unlock(&pool->queue_mutex);
		return TPOOL_ERR_HAS_TASKS;
	}

	// Установить флаг завершения
	pool->is_shutting_down = true;

	// Пробудить все ожидающие потоки
	pthread_cond_broadcast(&pool->queue_cond);
	pthread_mutex_unlock(&pool->queue_mutex);

	// Дождаться завершения всех потоков
	for (int i = 0; i < pool->active_thread_count; i++)
		pthread_join(pool->threads[i], NULL);

	// Освободить ресурсы
	pthread_mutex_destroy(&pool->queue_mutex);
	pthread_cond_destroy(&pool->queue_cond);
	free(pool->threads);
	free(pool);

	return 0;
}

/** Рабочая функция потока, который обрабатывает задачи из пула.
 *
 * Выполняет задачи из очереди, пока пул не начнет завершение.
 * @param arg Указатель на объект пула потоков.
 * @retval Всегда NULL.
 */
static void *worker_thread_function(void *thread_pool)
{
	struct thread_pool *pool = thread_pool;

	while (true)
	{
		pthread_mutex_lock(&pool->queue_mutex);

		// Ждать, пока появится задача или пул не начнет удаление
		while (pool->task_queue_head == NULL && !pool->is_shutting_down)
			pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);

		// Завершение: пул закрывается
		if (pool->is_shutting_down)
		{
			pthread_mutex_unlock(&pool->queue_mutex);
			break;
		}

		// Извлечь задачу из начала очереди
		struct thread_task *task = pool->task_queue_head;
		pool->task_queue_head = task->next;
		if (pool->task_queue_head == NULL)
			pool->task_queue_tail = NULL;

		task->next = NULL;
		pool->active_task_count--;

		pthread_mutex_unlock(&pool->queue_mutex);

		// Выполнить задачу
		pthread_mutex_lock(&task->mutex);
		task->state = TASK_STATE_RUNNING;
		pthread_mutex_unlock(&task->mutex);

		// Выполнить функцию задачи
		void *result = task->function(task->arg);

		// Сохранить результат и завершить задачу
		pthread_mutex_lock(&task->mutex);
		task->result = result;
		bool is_detached = (task->state == TASK_STATE_DETACHED);
		task->state = TASK_STATE_FINISHED;
		pthread_cond_broadcast(&task->cond);
		pthread_mutex_unlock(&task->mutex);

		// Если задача отсоединена — удалить её
		if (is_detached)
		{
			pthread_mutex_destroy(&task->mutex);
			pthread_cond_destroy(&task->cond);
			free(task);
		}
	}

	return NULL;
}

int thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	pthread_mutex_lock(&pool->queue_mutex);

	// Превышен лимит задач
	if (pool->active_task_count >= TPOOL_MAX_TASKS)
	{
		pthread_mutex_unlock(&pool->queue_mutex);
		return TPOOL_ERR_TOO_MANY_TASKS;
	}

	// Добавляем задачу в очередь
	task->next = NULL;
	task->state = TASK_STATE_IN_POOL;
	pool->active_task_count++;

	if (pool->task_queue_tail == NULL)
	{
		pool->task_queue_head = task;
		pool->task_queue_tail = task;
	}
	else
	{
		pool->task_queue_tail->next = task;
		pool->task_queue_tail = task;
	}

	// Запускаем новый поток, если их нет
	if (pool->active_thread_count < pool->max_thread_count &&
		pool->active_thread_count == 0)
	{
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker_thread_function, pool) == 0)
			pool->threads[pool->active_thread_count++] = thread;
	}

	pthread_cond_signal(&pool->queue_cond);
	pthread_mutex_unlock(&pool->queue_mutex);

	return 0;
}

int thread_task_new(struct thread_task **task, thread_task_f function, void *arg)
{
	struct thread_task *new_task = malloc(sizeof(struct thread_task));
	if (new_task == NULL)
		return TPOOL_ERR_NO_MEMORY;

	new_task->function = function;
	new_task->arg = arg;
	new_task->result = NULL;

	pthread_mutex_init(&new_task->mutex, NULL);
	pthread_cond_init(&new_task->cond, NULL);

	new_task->state = TASK_STATE_NEW;
	new_task->next = NULL;

	*task = new_task;

	return 0;
}

bool thread_task_is_finished(const struct thread_task *task)
{
	pthread_mutex_lock((pthread_mutex_t *)&task->mutex);
	bool result = (task->state == TASK_STATE_FINISHED);
	pthread_mutex_unlock((pthread_mutex_t *)&task->mutex);
	return result;
}

bool thread_task_is_running(const struct thread_task *task)
{
	pthread_mutex_lock((pthread_mutex_t *)&task->mutex);
	bool result = (task->state == TASK_STATE_RUNNING);
	pthread_mutex_unlock((pthread_mutex_t *)&task->mutex);
	return result;
}

int thread_task_join(struct thread_task *task, void **result)
{
	pthread_mutex_lock(&task->mutex);

	// Если задача не была передана в пул — ошибка
	if (task->state == TASK_STATE_NEW)
	{
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	// Ждём завершения задачи
	while (task->state != TASK_STATE_FINISHED)
		pthread_cond_wait(&task->cond, &task->mutex);

	// Задача завершена — возвращаем результат
	if (result != NULL)
		*result = task->result;

	pthread_mutex_unlock(&task->mutex);

	return 0;
}

#if NEED_TIMED_JOIN

int thread_task_timed_join(struct thread_task *task, double timeout, void **result)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)task;
	(void)timeout;
	(void)result;
	return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif

int thread_task_delete(struct thread_task *task)
{
	pthread_mutex_lock(&task->mutex);

	// Удалять можно только если задача ещё не в пуле или уже завершена
	if (task->state == TASK_STATE_IN_POOL || task->state == TASK_STATE_RUNNING)
	{
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_IN_POOL;
	}

	pthread_mutex_unlock(&task->mutex);

	pthread_mutex_destroy(&task->mutex);
	pthread_cond_destroy(&task->cond);
	free(task);

	return 0;
}

#if NEED_DETACH

int thread_task_detach(struct thread_task *task)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)task;
	return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif