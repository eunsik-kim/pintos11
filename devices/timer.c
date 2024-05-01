#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;
// 틱 값
static int curr_ticks;
static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt.
   8254 PIT를 초기화함. PIT를 설정하여 일정 주기로 인터럽트를 발생시킴.
   count 변수는 PIT가 발생시키는 인터럽트 주기를 결정한다.*/
void timer_init(void)
{
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ; // 1초에 몇번 인터럽트를 실행할것인가를 말하는것

	outb(0x43, 0x34);		  /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb(0x40, count & 0xff); // 포트에 데이터를 전송
	outb(0x40, count >> 8);

	intr_register_ext(0x20, timer_interrupt, "8254 Timer"); // 껐다가 timer_interrupt올리고 다시 키고를 반복
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void timer_calibrate(void)
{
	unsigned high_bit, test_bit;

	/* 인터럽트가 활성화되어 있는지 확인합니다.
	 * 타이머 보정은 인터럽트가 활성화된 상태에서만 수행됩니다. */
	ASSERT(intr_get_level() == INTR_ON);

	/* 타이머 보정 시작 메시지를 출력합니다. */
	printf("Calibrating timer...  ");

	/* 초기 루프 수를 2^10(=1024)로 설정합니다.
	 * 이 값은 타이머 인터럽트 주기에 근사한 값입니다. */
	loops_per_tick = 1u << 10;

	/* 지정된 루프 수를 사용하여 타이머 인터럽트 주기가 너무 짧지 않은지 확인합니다.
	 * 너무 짧으면 추가로 루프를 실행하여 보정해야 합니다. */
	while (!too_many_loops(loops_per_tick << 1))
	{
		loops_per_tick <<= 1;		 /* 루프 수를 두 배로 증가시킵니다. */
		ASSERT(loops_per_tick != 0); /* 0으로 나누는 것을 방지하기 위해 0이 아닌지 확인합니다. */
	}

	/* 루프 수를 보정하기 위해 다음 8비트를 조정합니다.
	 * 이를 통해 타이머 인터럽트 주기에 더 정확하게 근사할 수 있습니다. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
	{
		/* 현재 루프 수에 테스트 비트를 추가하여 타이머 인터럽트 주기를 확인합니다.
		 * 타이머 인터럽트 주기를 초과하면 추가로 루프를 실행하여 보정해야 합니다. */
		if (!too_many_loops(high_bit | test_bit))
			loops_per_tick |= test_bit;
	}

	/* 타이머의 루프 수가 보정되었으므로,
	 * 초당 루프 실행 횟수를 출력하여 타이머가 얼마나 정확한지 확인합니다. */
	printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t timer_ticks(void)
{
	/* 인터럽트를 비활성화하고 이전 인터럽트 레벨을 저장합니다.
	 * 이는 타이머 틱 수를 안전하게 읽기 위해 수행됩니다.
	 인터럽트의 비활성화는 프로그램 실행을 중단하는 것이 아닌, CPU가 하드웨어 인터럽트에 응답하지 않도록 하는것임*/
	enum intr_level old_level = intr_disable();

	/* 전역 변수인 ticks를 읽어옵니다.
	 * ticks는 OS 부팅 이후 경과한 타이머 틱 수를 나타냅니다. */
	int64_t t = ticks;

	/* 이전 인터럽트 레벨을 복원합니다. */
	intr_set_level(old_level);

	/* 컴파일러에게 코드 재배치를 방지하도록 지시합니다. */
	barrier();

	/* 타이머 틱 수를 반환합니다. */
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t timer_elapsed(int64_t then)
{
	/* 현재 타이머 틱 수와 THEN 이후의 틱 수를 비교하여
	 * 경과한 시간을 계산합니다. */
	return timer_ticks() - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
void timer_sleep(int64_t ticks)
{
	enum intr_level old_level = intr_disable();
	/* 실행을 중단하기 전에 시작 시간을 기록합니다. */
	int64_t start = timer_ticks();
	/* 인터럽트가 활성화되어 있는지 확인합니다.
	 * 타이머 중단은 인터럽트가 활성화된 상태에서만 수행됩니다. */
	// [Amborsia]
	curr_ticks = ticks + start;
	thread_sleep(curr_ticks);
}

/* Suspends execution for approximately MS milliseconds. */
void timer_msleep(int64_t ms)
{
	real_time_sleep(ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void timer_usleep(int64_t us)
{
	real_time_sleep(us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void timer_nsleep(int64_t ns)
{
	real_time_sleep(ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void timer_print_stats(void)
{
	printf("Timer: %" PRId64 " ticks\n", timer_ticks());
}

/* Timer interrupt handler. */
static void
timer_interrupt(struct intr_frame *args UNUSED)
{
	ticks++;
	thread_tick();
	//[Amborsia]
	thread_wakeup(ticks);
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops(unsigned loops)
{
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait(loops);

	/* If the tick count changed, we iterated too long. */
	barrier();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait(int64_t loops)
{
	while (loops-- > 0)
		barrier();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep(int64_t num, int32_t denom)
{
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT(intr_get_level() == INTR_ON);
	if (ticks > 0)
	{
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep(ticks);
	}
	else
	{
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT(denom % 1000 == 0);
		busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
