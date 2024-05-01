#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "list.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 `magic' 멤버에 대한 무작위 값.
   스택 오버플로우를 감지하기 위해 사용됨.  자세한 내용은
   thread.h 파일 맨 위의 큰 주석 참조. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 쓰레드용 무작위 값
   이 값을 수정하지 마십시오. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태의 프로세스 목록. 즉, 실행 준비가 된 프로세스
   실제로 실행 중이지는 않음. */
static struct list ready_list;

/* 휴식 리스트 */

static struct list blocked_list;

/* Idle thread. */
static struct thread *idle_thread;

/* 초기 쓰레드, init.c:main()에서 실행 중인 쓰레드. */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* 통계. */
static long long idle_ticks;   /* 유휴 시간에 소비된 타이머 틱 수. */
static long long kernel_ticks; /* 커널 쓰레드에서 소비된 타이머 틱 수. */
static long long user_ticks;   /* 사용자 프로그램에서 소비된 타이머 틱 수. */

/* 스케줄링. */
#define TIME_SLICE 4		  /* 각 쓰레드에 할당할 타이머 틱 수. */
static unsigned thread_ticks; /* 마지막 양보 이후의 타이머 틱 수. */

/* false(기본값)인 경우 라운드 로빈 스케줄러를 사용합니다.
   true인 경우 다단계 피드백 큐 스케줄러를 사용합니다.
   커널 명령줄 옵션 "-o mlfqs"에 의해 제어됩니다. */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

// [Amborsia] 만든 함수 처리
bool thread_less_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
bool thread_compare_donate_priority(const struct list_elem *l,
									const struct list_elem *s, void *aux UNUSED);
/* T가 유효한 쓰레드를 가리키는지 여부를 반환합니다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 실행 중인 쓰레드를 반환합니다.
 * CPU의 스택 포인터 `rsp`를 읽은 다음
 * 페이지의 시작 부분으로 내립니다. `struct thread`가
 * 항상 페이지의 시작 부분에 있고 스택 포인터는
 * 중간에 있기 때문에, 이는 현재 쓰레드를 찾습니다. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// 쓰레드 시작을 위한 전역 설명자 테이블.
// gdt는 thread_init 이후에 설정될 것이므로,
// 먼저 임시 gdt를 설정해야 합니다.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* 쓰레딩 시스템을 초기화합니다.
   현재 실행 중인 코드를 쓰레드로 변환하여 초기화합니다.
   이는 일반적으로 작동하지 않으며, 여기서만 가능합니다.
   loader.S가 스택의 맨 아래를 페이지 경계로 놓도록 주의했기 때문입니다.

   또한 실행 대기열과 tid 락을 초기화합니다.

   이 함수를 호출한 후에는
   thread_current()를 호출하기 전에
   페이지 할당기를 초기화해야합니다.

   이 함수가 완료될 때까지 thread_current()를 호출하는 것은 안전하지 않습니다. */
void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	/* 커널을 위한 임시 gdt 다시 로드
	 * 이 gdt에는 사용자 컨텍스트가 포함되지 않습니다.
	 * 커널은 gdt_init()에서 사용자 컨텍스트와 함께 gdt를 재구축할 것입니다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof(gdt) - 1,
		.address = (uint64_t)gdt};
	lgdt(&gdt_ds);

	/* 전역 쓰레드 컨텍스트 초기화 */
	lock_init(&tid_lock);
	list_init(&ready_list);		 // 준비리스트 초기화
	list_init(&blocked_list);	 // [Amborsia] 블럭 리스트 초기화
	list_init(&destruction_req); // 파괴리스트 초기화

	/* 실행 중인 쓰레드에 대한 스레드 구조체 설정 */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING; // init_thread에서는 block이었는데 running으로 바꿔줌
	initial_thread->tid = allocate_tid();	 // allocate_tid를 통해서 고유한 tid값을 계속해서 올려줌
}

/* 인터럽트를 사용하여 선점형 쓰레드 스케줄링을 시작합니다.
   또한 idle 쓰레드를 생성합니다. */
void thread_start(void)
{
	/* idle 쓰레드 생성 */
	struct semaphore idle_started; // 운영체제가 할일이 없을때 실행되는 특별한 쓰레드
	sema_init(&idle_started, 0);   // 쓰레드이기 때문에 쎄마포어 초기화를 시켜줌
	thread_create("idle", PRI_MIN, idle, &idle_started);

	/* 선점형 쓰레드 스케줄링 시작 */
	intr_enable();

	/* idle 쓰레드가 0으로 계속 기다리다가 무언가 값이 올랐을때 실행이 됨 */
	sema_down(&idle_started);
}

/* 타이머 인터럽트 핸들러에 의해 각 타이머 틱마다 호출됩니다.
   따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됩니다. */
void thread_tick(void)
{
	struct thread *t = thread_current();

	/* 통계 업데이트 */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* 선점 적용 time_slice가 스케쥴링의 기법 중 하나임 */
	if (++thread_ticks >= TIME_SLICE) // 쓰레드를 양보한 이후 4틱이 지나면 함수 실행
		intr_yield_on_return();
}

/* 쓰레드 통계를 출력합니다. */
void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
		   idle_ticks, kernel_ticks, user_ticks);
}

/* 이름이 NAME이고 초기 우선순위가 주어진 새 커널 쓰레드를 생성하고
   AUX를 인수로 전달하여 FUNCTION을 실행하고 준비 큐에 추가합니다.
   새 쓰레드에 대한 쓰레드 식별자를 반환하거나
   생성에 실패하면 TID_ERROR를 반환합니다.

   thread_start()가 호출되었다면, 새 쓰레드가 반환되기 전에
   thread_create()가 반환되기 전에 예약될 수 있습니다. 그것은 심지어 종료
   되기 전에 thread_create()를 반환 할 수도 있습니다.
   순서를 보장해야 하는 경우 세마포어나 다른 형태의 동기화를 사용하십시오.

   제공된 코드는 새 쓰레드의 `priority' 멤버를
   PRIORITY로 설정하지만, 실제로는 우선순위 스케줄링이 구현되어 있지 않습니다.
   우선순위 스케줄링은 문제 1-3의 목표입니다. */
tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t;
	tid_t tid;

	ASSERT(function != NULL);

	/* 쓰레드 할당 */
	t = palloc_get_page(PAL_ZERO); // 스레드의 스택이 4kb로 운영되어서 그만큼을 할당해줌
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread(t, name, priority);
	tid = t->tid = allocate_tid();

	/* 스케줄되면 kernel_thread 호출
	 * 참고) rdi는 1번째 인수이고 rsi는 2번째 인수입니다. */
	t->tf.rip = (uintptr_t)kernel_thread; // rip에서 이 함수를 가리키고 있으므로 context switching이 일어날때 kernel_thread함수가 실행이된다.
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* 실행 대기열에 추가 */
	thread_unblock(t);
	/* [Amborsia] 우선순위 높은 것 실행을 위한 작업 */
	if (thread_current()->priority < priority)
	{
		thread_yield();
	}
	return tid;
}

/* 현재 쓰레드를 슬립 상태로 만듭니다.
   thread_unblock()에 의해 깨어날 때까지 다시 스케줄되지 않습니다.

   이 함수는 인터럽트가 비활성화된 상태에서 호출되어야 합니다.
   일반적으로 synch.h의 동기화 기본 형식을 사용하는 것이 더 좋습니다. */
void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);
	thread_current()->status = THREAD_BLOCKED;
	schedule();
}

/* 블록된 쓰레드 T를 실행 준비 상태로 전환합니다.
   T가 블록되지 않았으면 이것은 에러입니다. (호출자가 직접
   쓰레드를 준비 상태로 만들려면 thread_yield()를 사용하십시오.)

   이 함수는 실행 중인 쓰레드를 선점하지 않습니다. 이것은 중요합니다.
   호출자가 인터럽트를 직접 비활성화하고 있으면
   쓰레드를 원자적으로 블록 해제하고
   다른 데이터를 업데이트할 수 있다고 예상할 수 있습니다. */
void thread_unblock(struct thread *t)
{
	enum intr_level old_level = intr_disable();

	ASSERT(is_thread(t));

	ASSERT(t->status == THREAD_BLOCKED);
	// [Amborsia] 리스트 내림차순으로 적용
	list_insert_ordered(&ready_list, &t->elem, thread_less_priority, NULL);
	t->status = THREAD_READY;
	intr_set_level(old_level); // interrupt를 실행레벨로 만듦
}

/* 실행 중인 쓰레드의 이름을 반환합니다. */
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* 실행 중인 쓰레드를 반환합니다.
   이것은 running_thread()에 몇 가지 안전 검사를 추가한 것입니다.
   자세한 내용은 thread.h 맨 위의 큰 주석을 참조하십시오. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* T가 실제로 쓰레드인지 확인합니다.
   이러한 단언 중 하나라도 작동하지 않으면
   쓰레드가 스택을 오버플로우한 것입니다.
   각 쓰레드의 스택은 4 KB 미만이므로
   큰 자동 배열이나 중간 정도의 재귀로 인해
   스택 오버플로우가 발생할 수 있습니다. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* 실행 중인 쓰레드의 TID를 반환합니다. */
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* 현재 쓰레드를 종료합니다.
   이 함수는 실행 중인 쓰레드에 대해서만 호출됩니다.
   실행 중인 쓰레드는 종료됩니다. 쓰레드가 반환되면
   프로세스를 종료합니다. */
void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	/* 상태를 죽어가는 중으로 설정하고 다른 프로세스를 예약하면 됩니다.
	   schedule_tail()을 호출하는 동안 소멸됩니다. */
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

/* CPU를 반환합니다.  현재 스레드는 절전 모드로 전환되지 않으며 스케줄러의 임의로 즉시 다시 스케줄링할 수 있습니다. */
void thread_yield(void)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());
	// [Amborsia] 양보할 경우도 내림차순 정렬
	old_level = intr_disable();
	if (curr != idle_thread)
		// list_push_back(&ready_list, &curr->elem);

		list_insert_ordered(&ready_list, &curr->elem, thread_less_priority, NULL);
	do_schedule(THREAD_READY);
	intr_set_level(old_level);
}

// [Amborsia] 내림차순 관련 함수
bool thread_less_priority(const struct list_elem *a_, const struct list_elem *b_, void *aux UNUSED)
{
	struct thread *a = list_entry(a_, struct thread, elem);
	struct thread *b = list_entry(b_, struct thread, elem);
	return a->priority > b->priority;
}

// [Amborsia] 기부 관련 함수
bool thread_compare_donate_priority(const struct list_elem *l,
									const struct list_elem *s, void *aux UNUSED)
{
	return list_entry(l, struct thread, donation_elem)->priority > list_entry(s, struct thread, donation_elem)->priority;
}

// [Amborsia] 타이머 관련 함수
bool thread_less_func(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
	const struct thread *ta = list_entry(a, struct thread, elem);
	const struct thread *tb = list_entry(b, struct thread, elem);
	return ta->tick > tb->tick;
}
// [Amborsia] 틱을 체크해서 리스트 안에 넣어줌
void thread_sleep(int time)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	if (curr != idle_thread)
	{
		curr->tick = time;
		list_push_back(&blocked_list, &curr->sleep_elem);
		// list_insert_ordered(&blocked_list, curr->tick, thread_less_func, NULL);

		thread_block();
	}
	old_level = intr_enable();
}
// [Amborsia] 쓰레드가 자고 있는 것을 체크해서 깨워야할 시간에 깨우는 작업
// thread_wake_up은 인터럽트가 걸릴때마다 실행된다.
void thread_wakeup(int cur)
{
	struct list_elem *front = list_begin(&blocked_list);
	struct thread *t;

	while (front != list_end(&blocked_list))
	{
		t = list_entry(front, struct thread, sleep_elem);
		if (t->tick <= cur)
		{
			thread_unblock(t);
			front = list_remove(front);
		}
		else
		{
			front = list_next(front);
		}
	}
}

/* 현재 쓰레드의 우선 순위를 NEW_PRIORITY로 설정합니다. */
void thread_set_priority(int new_priority)
{
	// [Amborsia] donation을 위해서 현재의 초기 우선순위를 설정 및 갱신
	thread_current()->init_priority = new_priority;
	refresh_priority();
	// thread_test_preemption();
	if (thread_current()->priority < list_entry(list_begin(&ready_list), struct thread, elem)->priority)
	{
		thread_yield();
	}
}

/* 현재 쓰레드의 우선 순위를 반환합니다. */
int thread_get_priority(void)
{
	return thread_current()->priority;
}

/* 현재 쓰레드의 nice 값을 NICE로 설정합니다. */
void thread_set_nice(int nice UNUSED)
{
	printf("test1\n");
	/* TODO: Your implementation goes here */
}

/* 현재 쓰레드의 nice 값을 반환합니다. */
int thread_get_nice(void)
{
	printf("test2\n");
	/* TODO: Your implementation goes here */
	return 0;
}

/* 시스템 로드 평균의 100배를 반환합니다. */
int thread_get_load_avg(void)
{
	printf("test3\n");
	/* TODO: Your implementation goes here */
	return 0;
}

/* 현재 쓰레드의 recent_cpu 값의 100배를 반환합니다. */
int thread_get_recent_cpu(void)
{
	printf("test4\n");
	/* TODO: Your implementation goes here */
	return 0;
}

// [Amborsia] 우선순위 기부를 위해서 적용해둠 holder의 우선순위를 현재 우선순위로 바꿔줌
void donate_priority(void)
{
	int depth;
	struct thread *cur = thread_current();

	// 재귀적으로 최대 8단계까지 우선순위를 기부합니다.
	for (depth = 0; depth < 8; depth++)
	{
		// 만약 현재 스레드가 어떤 락을 기다리지 않는다면 우선순위 기부를 종료합니다.
		if (!cur->wait_on_lock)
		{
			// wait_on_lock == NULL이라면 더이상 donation 을 진행하지 않아도 되기 때문에 break;
			break;
		}
		// 현재 스레드가 기다리는 락의 소유자를 가져옵니다.
		struct thread *holder = cur->wait_on_lock->holder;
		// 락의 소유자의 우선순위를 현재 스레드의 우선순위로 업데이트합니다.
		holder->priority = cur->priority;
		// 현재 스레드를 락의 소유자로 변경합니다.
		cur = holder;
	}
}
// [Amborsia] 기부 받은 리스트를 삭제하는 작업
void remove_with_lock(struct lock *lock)
{
	struct list_elem *e;
	struct thread *cur = thread_current();

	// 현재 스레드의 기부 리스트를 순회합니다.
	for (e = list_begin(&cur->donations); e != list_end(&cur->donations); e = list_next(e))
	{
		// 현재 기부 노드에서 연결된 스레드를 가져옵니다.
		struct thread *t = list_entry(e, struct thread, donation_elem);
		// 만약 해당 스레드가 특정 락을 기다리고 있다면
		if (t->wait_on_lock == lock)
		{
			// 기부 노드를 기부 리스트에서 제거합니다.
			list_remove(&t->donation_elem);
		}
	}
}

// [Amborsia] 기부를 받거나 반납했거나 했을때 다시 원래대로 돌아가야하기 때문에
// 이 작업을 위해서 refresh를 처리해줘야함
void refresh_priority(void)
{
	struct thread *cur = thread_current();

	// 스레드의 우선순위를 초기 우선순위로 초기화합니다.
	cur->priority = cur->init_priority;

	// 만약 스레드가 다른 스레드로부터 우선순위 기부를 받았다면
	if (!list_empty(&cur->donations))
	{
		// 우선순위 기부 리스트를 기부 우선순위에 따라 정렬합니다.
		list_sort(&cur->donations, thread_compare_donate_priority, 0);

		// 기부 리스트의 가장 앞에 있는 스레드의 우선순위를 확인합니다.
		struct thread *front = list_entry(list_front(&cur->donations), struct thread, donation_elem);
		// 만약 기부 받은 우선순위가 현재 우선순위보다 높다면
		if (front->priority > cur->priority)
		{
			// 스레드의 우선순위를 기부 받은 우선순위로 업데이트합니다.
			cur->priority = front->priority;
		}
	}
}

/* 아이들 쓰레드. 다른 쓰레드가 준비되지 않은 경우 실행됩니다.

   아이들 쓰레드는 처음에 준비 목록에 넣고
   thread_start()에서 호출됩니다. 초기에 한 번만 예약됩니다.
   그런 다음 아이들 쓰레드는 초기화되고, 넘겨진 세마포어를 초기화합니다.
   그러고 나서 바로 차단됩니다.
   이후 아이들 쓰레드는 준비 목록에 나타나지 않습니다. 준비 목록이 비어 있는 경우
   특수 케이스로 next_thread_to_run()에서 반환됩니다. */
static void
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		/* 누군가가 실행할 수 있도록 합니다. */
		intr_disable();
		thread_block();
		/* 인터럽트를 다시 활성화하고 다음 인터럽트를 기다립니다.

				   'sti' 명령은 다음 명령이 완료될 때까지 인터럽트를 비활성화합니다.
				   따라서 이 두 명령은 원자적으로 실행됩니다. 이러한 원자성은
				   중요합니다. 그렇지 않으면 인터럽트가 처리될 수 있습니다.
				   인터럽트를 다시 활성화하고 다음 인터럽트를 기다리는 동안
				   시간당 최대 한 클럭 틱의 시간이 소비됩니다.

				   [IA32-v2a] "HLT", [IA32-v2b] "STI" 및 [IA32-v3a]를 참조하십시오.
				   7.11.1 "HLT Instruction". */
		asm volatile("sti; hlt" : : : "memory");
	}
}

/* 커널 쓰레드의 기초가 되는 함수. */
static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable(); /* 스케줄러는 인터럽트를 사용할 수 있도록 풀어줌. */
	function(aux); /* 쓰레드 함수 실행 */
	thread_exit(); /* function()이 반환되면 쓰레드를 종료합니다. */
}

/* T를 NAME으로 이름 지어 PRI의 초기 값으로 초기화합니다. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name); // t->name만큼의 크기만큼만 복사해서 넣어줌
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC; // 스택 영역의 끝값을 가리키고 있음
	/* [Amborsia] donation관련 초기화 함수 */
	t->init_priority = priority;
	t->wait_on_lock = NULL;
	list_init(&t->donations);
}

/* 스케줄할 다음 쓰레드를 선택하고 반환합니다.
   실행 대기열에서 쓰레드를 반환해야 합니다. 실행 대기열이
   비어있으면 idle_thread를 반환합니다. */
static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf)
{
	__asm __volatile(
		"movq %0, %%rsp\n"
		"movq 0(%%rsp),%%r15\n"
		"movq 8(%%rsp),%%r14\n"
		"movq 16(%%rsp),%%r13\n"
		"movq 24(%%rsp),%%r12\n"
		"movq 32(%%rsp),%%r11\n"
		"movq 40(%%rsp),%%r10\n"
		"movq 48(%%rsp),%%r9\n"
		"movq 56(%%rsp),%%r8\n"
		"movq 64(%%rsp),%%rsi\n"  // 2nd
		"movq 72(%%rsp),%%rdi\n"  // 1st
		"movq 80(%%rsp),%%rbp\n"  // callee
		"movq 88(%%rsp),%%rdx\n"  // 3rd
		"movq 96(%%rsp),%%rcx\n"  // 4th
		"movq 104(%%rsp),%%rbx\n" // callee
		"movq 112(%%rsp),%%rax\n" // return
		"addq $120,%%rsp\n"
		"movw 8(%%rsp),%%ds\n"
		"movw (%%rsp),%%es\n"
		"addq $32, %%rsp\n"
		"iretq"
		: : "g"((uint64_t)tf) : "memory");
}

/* 새 스레드의 페이지
   테이블을 활성화하고, 이전 스레드가 죽어가는 경우 소멸시킵니다.

   이 함수를 호출할 때 방금 스레드에서 전환했습니다.
   PREV에서 새 스레드가 이미 실행 중이며 인터럽트는
   여전히 비활성화되어 있습니다.

   따라서 스레드 전환이
   완료될 때까지는 호출하는 것이 안전하지 않습니다.  실제로는 함수 끝에 printf()를
   를 함수 끝에 추가해야 합니다.  */
static void
thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	/*주요 스위칭 로직입니다.
	 * 먼저 전체 실행 컨텍스트를 intr_frame에 복원하고
	 * 로 전체 실행 컨텍스트를 복원하고 do_iret을 호출하여 다음 스레드로 전환합니다.
	 * 여기서부터 스택을 사용해서는 안 됩니다.
	 * 전환이 완료될 때까지 스택을 사용해서는 안 됩니다.
	 * rip => 코드 몇째줄을 실행하고 있는지를 체크
	 * stepi를 통해서 한줄한줄 실행이 가능함*/
	__asm __volatile(
		/* Store registers that will be used. */
		"push %%rax\n"
		"push %%rbx\n"
		"push %%rcx\n"
		/* Fetch input once */
		"movq %0, %%rax\n"
		"movq %1, %%rcx\n"
		"movq %%r15, 0(%%rax)\n"
		"movq %%r14, 8(%%rax)\n"
		"movq %%r13, 16(%%rax)\n"
		"movq %%r12, 24(%%rax)\n"
		"movq %%r11, 32(%%rax)\n"
		"movq %%r10, 40(%%rax)\n"
		"movq %%r9, 48(%%rax)\n"
		"movq %%r8, 56(%%rax)\n"
		"movq %%rsi, 64(%%rax)\n"
		"movq %%rdi, 72(%%rax)\n"
		"movq %%rbp, 80(%%rax)\n"
		"movq %%rdx, 88(%%rax)\n"
		"pop %%rbx\n" // Saved rcx
		"movq %%rbx, 96(%%rax)\n"
		"pop %%rbx\n" // Saved rbx
		"movq %%rbx, 104(%%rax)\n"
		"pop %%rbx\n" // Saved rax
		"movq %%rbx, 112(%%rax)\n"
		"addq $120, %%rax\n"
		"movw %%es, (%%rax)\n"
		"movw %%ds, 8(%%rax)\n"
		"addq $32, %%rax\n"
		"call __next\n" // read the current rip.
		"__next:\n"
		"pop %%rbx\n"
		"addq $(out_iret -  __next), %%rbx\n"
		"movq %%rbx, 0(%%rax)\n" // rip
		"movw %%cs, 8(%%rax)\n"	 // cs
		"pushfq\n"
		"popq %%rbx\n"
		"mov %%rbx, 16(%%rax)\n" // eflags
		"mov %%rsp, 24(%%rax)\n" // rsp
		"movw %%ss, 32(%%rax)\n"
		"mov %%rcx, %%rdi\n"
		"call do_iret\n"
		"out_iret:\n"
		: : "g"(tf_cur), "g"(tf) : "memory");
}

/* 새 프로세스를 스케줄합니다. 진입시, 인터럽트가 비활성화되어야 합니다.
 * 이 함수는 현재 쓰레드의 상태를 status로 변경한 다음 다른 쓰레드를 찾아
 * 그것으로 전환합니다.
 * schedule()에서 printf()를 호출하는 것은 안전하지 않습니다. */
static void
do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);
	while (!list_empty(&destruction_req))
	{
		struct thread *victim =
			list_entry(list_pop_front(&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current()->status = status;
	schedule();
}

// [Amborsia] 우선순위 비교를 해서 양보를 처리하는 방식
void thread_test_preemption(void)
{
	if (!list_empty(&ready_list))
	{
		struct thread *a = list_entry(list_front(&ready_list), struct thread, elem);
		if (thread_current()->priority < a->priority)
		{
			thread_yield();
		}
	}
}

static void
schedule(void)
{
	struct thread *curr = running_thread();
	struct thread *next = next_thread_to_run();

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(curr->status != THREAD_RUNNING);
	ASSERT(is_thread(next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next);
#endif

	if (curr != next)
	{
		/* 전환한 스레드가 죽어가고 있으면 해당 스레드의 구조체
		   스레드를 파괴합니다. 이 작업은 늦게 이루어져야 thread_exit()가
		   스스로 깔개를 꺼내지 않도록 늦게 발생해야 합니다.
		   여기서는 페이지가 현재 스택에서 사용 중이므로
		   페이지가 현재 스택에서 사용되고 있기 때문입니다.
		   실제 소멸 로직은 스택의 시작 부분에서
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch(next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}
