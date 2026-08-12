# What This Linker Script Does (Simple Version)

Think of your microcontroller as having two "boxes" to put things in:

- **Flash** — 64 KB, starts at address `0x08000000`. This is permanent storage, like a hard drive. Your program code lives here and survives power-off.
- **RAM** — 20 KB, starts at address `0x20000000`. This is temporary/working memory, like scratch paper. It's wiped every time the chip loses power.

The linker script's job: tell the tool that builds your final program **where in Flash and RAM everything should go.**

---

## The first two lines

```
EXTERN(vector_table)
ENTRY(reset_handler)
```

- `vector_table` is a list of addresses the chip needs at the very start of Flash (interrupt handlers, etc). This line just makes sure it doesn't get accidentally deleted by the linker.
- `reset_handler` is the function that runs first when the chip powers on or resets. This line tells tools "this is where the program starts."

You don't need to touch these — just know they exist to protect the boot process.

---

## The MEMORY section — declaring the two boxes

```
MEMORY {
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
    rom (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
}
```

This is literally just: "Flash is here, this big. RAM is here, this big."
`rx` means readable+executable (code can run from Flash). `rwx` means RAM can also be written to (variables change).

---

## The SECTIONS block — deciding what goes where

This is the long part, but it boils down to **4 simple categories**:

| What | Goes in | Why |
|---|---|---|
| Your program's actual code | Flash | Code doesn't change, so it stays in permanent storage |
| Constant values (e.g. `const int x = 5`) | Flash | Also never changes |
| Variables that start with a value (e.g. `int x = 5;`, not const) | Flash **and** RAM | The starting value `5` is stored in Flash, then copied into RAM at startup so the program can change it while running |
| Variables that start empty (e.g. `int x;`) | RAM only | No need to store anything in Flash — the startup code just zeroes out RAM for these |

That's really the whole point of most of this script. Let's go through the actual named sections:

### `.text` → Flash
Your compiled code, plus the vector table, plus any `const` data. All permanent, all read-only, all lives in Flash.

### `.preinit_array`, `.init_array`, `.fini_array` → Flash
Only relevant if you use C++ or `__attribute__((constructor))` in C. These are lists of "run this function automatically at startup/shutdown." Most simple C projects can ignore this — it'll just be empty.

### `.ARM.extab`, `.ARM.exidx` → Flash
Bookkeeping data for C++ exceptions (`try`/`catch`). If you're writing plain C with no exceptions, these end up empty too — they're just here so the build doesn't break if some library expects them.

### `.noinit` → RAM (special case)
Normally on reset, RAM gets cleared. Sometimes you want a small area that **survives a reset** — for example, a flag that says "the app crashed, boot into recovery mode instead." Anything you put in `.noinit` keeps its old value even after reset.

### `.data` → Flash *and* RAM
This is the "variables with a starting value" case explained above. The starting values are stored in Flash. At boot, the startup code copies them into RAM so your program can read/modify them while running.

### `.bss` → RAM only
"Variables with no starting value" (or explicitly `= 0`). Nothing needs to be stored in Flash for these — startup code just fills that chunk of RAM with zeros.

### `.eh_frame` → thrown away
```
/DISCARD/ : { *(.eh_frame) }
```
More C++ exception bookkeeping. This project doesn't need it, so it's deleted to save space.

---

## The stack

```
PROVIDE(_stack = ORIGIN(ram) + LENGTH(ram));
```

The **stack** is the memory used for function calls (local variables, return addresses). This line just says: "put the stack at the very top of RAM." The stack then grows *downward* from there as your program calls functions, while `.data`/`.bss` sit at the *bottom* of RAM and grow *upward*. As long as they don't meet in the middle, you're fine.

---

## Putting it all together: what happens at power-on

1. Chip reads the first two entries of the vector table from Flash: the starting stack address, and the address of `reset_handler`.
2. `reset_handler` runs. It:
   - Copies the `.data` starting values from Flash into RAM.
   - Zeros out the `.bss` area in RAM.
   - Runs any startup functions in `.init_array`.
   - Calls `main()`.
3. Your program runs, using RAM for variables and the stack, reading code straight out of Flash.

That's really all a linker script is doing here — deciding **which memory chip each piece of your program lives in**, and leaving breadcrumbs (`_data`, `_edata`, `_ebss`, etc.) so the startup code knows exactly where each chunk begins and ends.