# Simple Timer Notes

This note explains `shared/src/core/simple-timer.c` and
`shared/inc/core/simple-timer.h`.

This file implements a small **software timer**. It does not directly configure
STM32 hardware timers like TIM2 or TIM3. Instead, it uses the system tick count
from `system_get_ticks()` and checks whether enough time has passed.

## The files

Header file:

```c
#ifndef INC_SIMPLE_TIMER_H
#define INC_SIMPLE_TIMER_H
#include "common-defines.h"

typedef struct simple_timer_t{
   uint64_t wait_time;
   uint64_t target_time;
   bool auto_reset;

} simple_timer_t;

void simple_timer_setup(simple_timer_t* timer, uint64_t wait_time, bool auto_reset);
bool simple_timer_has_elapsed(simple_timer_t* timer);

void simple_timer_reset(simple_timer_t* timer);

#endif
```

Source file:

```c
#include "core/simple-timer.h"
#include "core/system.h"

void simple_timer_setup(simple_timer_t* timer, uint64_t wait_time, bool auto_reset){
timer ->wait_time=wait_time;
timer->auto_reset=auto_reset;
timer->target_time=system_get_ticks()+wait_time;
}
bool simple_timer_has_elapsed(simple_timer_t* timer){
uint64_t now = system_get_ticks();
bool has_elapsed = now >=timer->target_time;

if (has_elapsed && timer->auto_reset){
  uint64_t drift= now - timer->target_time;
  timer->target_time= (now+timer->wait_time)-drift;

}
return has_elapsed;
}

void simple_timer_reset(simple_timer_t* timer){
    simple_timer_setup(timer,timer->wait_time,timer->auto_reset);
}
```

## What problem this solves

In embedded systems, you often want to do something after a delay:

- blink an LED every 500 ms
- send a UART message every 1 second
- check a sensor every 100 ms
- retry communication after a timeout

One way is to block the CPU:

```c
system_delay(500);
```

That works, but the CPU waits there and cannot do other useful work during the
delay.

A software timer lets the program keep running while checking:

```text
Has enough time passed yet?
```

So the main loop can continue handling UART, packets, buttons, sensors, and
other tasks.

## Hardware timer vs software timer

A **hardware timer** is a peripheral inside the STM32. Examples are TIM1, TIM2,
TIM3, TIM4, etc. Hardware timers can count, generate PWM, capture pulses, and
trigger interrupts.

This `simple-timer.c` file is a **software timer**.

It depends on this function:

```c
system_get_ticks()
```

In this project, `system_get_ticks()` returns a millisecond counter maintained
by SysTick.

So this simple timer is basically:

```text
current time + wait time = target time
```

Then later:

```text
if current time >= target time, the timer has elapsed
```

## The timer struct

```c
typedef struct simple_timer_t{
   uint64_t wait_time;
   uint64_t target_time;
   bool auto_reset;

} simple_timer_t;
```

This struct stores the state of one timer.

| Field | Meaning |
|---|---|
| `wait_time` | How long the timer should wait |
| `target_time` | The tick value when the timer should expire |
| `auto_reset` | Whether the timer should automatically schedule the next period |

Because each timer has its own struct, you can create more than one timer:

```c
simple_timer_t led_timer;
simple_timer_t uart_timer;
simple_timer_t sensor_timer;
```

Each one can have a different delay.

## `wait_time`

```c
uint64_t wait_time;
```

This is the delay length.

If `system_get_ticks()` is counting milliseconds, then:

```c
wait_time = 1000;
```

means:

```text
wait 1000 ms = 1 second
```

## `target_time`

```c
uint64_t target_time;
```

This is the future tick count where the timer expires.

Example:

```text
current tick = 5000
wait_time    = 1000
target_time  = 6000
```

The timer has not elapsed at tick 5500.

The timer has elapsed at tick 6000 or later.

## `auto_reset`

```c
bool auto_reset;
```

This decides what happens after the timer expires.

If `auto_reset` is `false`:

- the timer expires once
- it stays expired until you manually reset it

If `auto_reset` is `true`:

- the timer expires
- then it automatically schedules the next expiry
- useful for repeated events like blinking an LED

## Function: setup

```c
void simple_timer_setup(simple_timer_t* timer, uint64_t wait_time, bool auto_reset){
timer ->wait_time=wait_time;
timer->auto_reset=auto_reset;
timer->target_time=system_get_ticks()+wait_time;
}
```

This initializes the timer.

It stores:

```c
timer->wait_time = wait_time;
```

This remembers how long the timer should wait.

It stores:

```c
timer->auto_reset = auto_reset;
```

This remembers whether the timer should repeat automatically.

Then it calculates:

```c
timer->target_time = system_get_ticks() + wait_time;
```

This means:

```text
expire at current time + wait time
```

Example:

```text
system_get_ticks() = 200
wait_time = 500
target_time = 700
```

The timer should elapse when the system tick reaches 700.

## Function: has elapsed

```c
bool simple_timer_has_elapsed(simple_timer_t* timer){
uint64_t now = system_get_ticks();
bool has_elapsed = now >=timer->target_time;
```

First, the code reads the current time:

```c
uint64_t now = system_get_ticks();
```

Then it checks whether the current time has reached the target:

```c
bool has_elapsed = now >= timer->target_time;
```

If `now` is smaller than `target_time`, the timer is still waiting.

If `now` is equal to or greater than `target_time`, the timer has elapsed.

## Auto reset behavior

```c
if (has_elapsed && timer->auto_reset){
  uint64_t drift= now - timer->target_time;
  timer->target_time= (now+timer->wait_time)-drift;
}
```

This block only runs when:

- the timer has elapsed
- `auto_reset` is enabled

Its job is to schedule the next expiry.

The simple idea would be:

```c
timer->target_time = now + timer->wait_time;
```

But this code also calculates drift:

```c
uint64_t drift = now - timer->target_time;
```

`drift` means:

```text
how late did we check the timer?
```

Example:

```text
target_time = 1000
now         = 1015
drift       = 15
```

The timer expired at 1000, but the program checked it at 1015.

Then the next target is calculated:

```c
timer->target_time = (now + timer->wait_time) - drift;
```

This is the same as:

```text
target_time + wait_time
```

because:

```text
(now + wait_time) - (now - old_target)
= old_target + wait_time
```

So if the timer was supposed to fire every 1000 ms:

```text
1000, 2000, 3000, 4000...
```

it tries to stay on that schedule instead of slowly drifting later and later.

## Function: reset

```c
void simple_timer_reset(simple_timer_t* timer){
    simple_timer_setup(timer,timer->wait_time,timer->auto_reset);
}
```

This restarts the timer using the same settings it already had.

It keeps:

- the same `wait_time`
- the same `auto_reset` setting

but creates a new target time based on the current tick.

Example:

```text
old wait_time = 500
old auto_reset = true
current tick = 1200
new target_time = 1700
```

## One-shot timer example

A one-shot timer expires once.

```c
simple_timer_t timeout;

simple_timer_setup(&timeout, 1000, false);

while (true) {
    if (simple_timer_has_elapsed(&timeout)) {
        /* 1 second has passed */
    }
}
```

Because `auto_reset` is `false`, after the timer expires,
`simple_timer_has_elapsed()` will keep returning `true` until you call:

```c
simple_timer_reset(&timeout);
```

## Repeating timer example

A repeating timer is useful for periodic tasks.

```c
simple_timer_t heartbeat;

simple_timer_setup(&heartbeat, 500, true);

while (true) {
    if (simple_timer_has_elapsed(&heartbeat)) {
        /* Runs about every 500 ms */
    }
}
```

Because `auto_reset` is `true`, the timer schedules the next 500 ms period by
itself.

## How this fits with `system.c`

This simple timer depends on:

```c
system_get_ticks()
```

In `shared/src/core/system.c`, SysTick is configured to interrupt every 1 ms.
Each SysTick interrupt increments a counter:

```c
void sys_tick_handler(void)
{
    ++ms_ticks;
}
```

Then:

```c
uint32_t system_get_ticks(void)
{
    return ms_ticks;
}
```

returns the current millisecond count.

So the simple timer is built on top of SysTick:

```text
SysTick interrupt -> ms_ticks increases -> simple_timer checks ms_ticks
```

## Important details

### This does not block

`simple_timer_has_elapsed()` only checks time and returns `true` or `false`.

It does not wait.

That means the rest of the program can keep running.

### The timer must be checked

This timer does not call your function automatically.

You must check it in a loop:

```c
if (simple_timer_has_elapsed(&timer)) {
    /* do the timed work */
}
```

### The units depend on `system_get_ticks()`

In this project, ticks are milliseconds.

So `wait_time = 1000` means 1000 ms.

If another project made `system_get_ticks()` count microseconds, then the same
timer would use microseconds instead.

### Pointer must be valid

The functions expect:

```c
timer != NULL
```

The current code does not check for `NULL`, so the caller must pass the address
of a real `simple_timer_t`.

Correct:

```c
simple_timer_t timer;
simple_timer_setup(&timer, 1000, true);
```

Wrong:

```c
simple_timer_setup(NULL, 1000, true);
```

## Key things to remember

- `simple_timer_t` stores one timer's state.
- `wait_time` is the delay.
- `target_time` is when the timer should expire.
- `auto_reset` decides whether the timer repeats.
- `simple_timer_setup()` starts the timer.
- `simple_timer_has_elapsed()` checks whether time is up.
- `simple_timer_reset()` restarts the timer with the same settings.
- This is a software timer built on top of `system_get_ticks()`.
- It is useful when you want timing without blocking the whole program.

## Related files

- [Simple timer implementation](../shared/src/core/simple-timer.c)
- [Simple timer header](../shared/inc/core/simple-timer.h)
- [System tick implementation](../shared/src/core/system.c)
- [General-purpose timer notes](general_purpose_timer.md)
