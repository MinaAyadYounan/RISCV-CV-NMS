/* Self-contained linker script for the bare-metal RISC-V-CV images.
 *
 * It reproduces nexus-am's loader64.ld + section.ld layout exactly, then adds
 * a .init_array section so C++ static constructors are linked in and
 * discoverable at runtime.
 *
 * Why not reuse section.ld via INSERT? nexus-am's section.ld predates C++ and
 * has no .init_array; passed as a second -T script with INSERT AFTER, the
 * linker processes this script before the referenced section exists and
 * errors out. Keeping a self-contained copy is simpler and stable.
 *
 * The __init_array_start/__init_array_end symbols are consumed by
 * riscv_run_static_ctors() in lib/source/riscv_runtime.cpp. Without
 * KEEP()ing .init_array, --gc-sections would discard every self-registered
 * gtest-lite test.
 */

pmem_base = 0x80000000;

MEMORY {
  ram (rwxa) : ORIGIN = 0x80000000, LENGTH = 1024M
}

ENTRY(_start)

SECTIONS {
  . = ORIGIN(ram);
  .text : {
    *(entry)
    *(.text)
    *(.text*)
  }
  etext = .;
  _etext = .;
  .rodata : {
    *(.rodata*)
  }
  /* C++ static constructors (gtest-lite self-registration). */
  .init_array : {
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  }
  .data : {
    *(.data)
  }
  edata = .;
  _data = .;
  .bss : {
    _bss_start = .;
    *(.bss*)
    *(.sbss*)
    *(.scommon)
  }
  _stack_top = ALIGN(0x1000);
  . = _stack_top + 0x8000;
  _stack_pointer = .;
  . = _stack_top + 0x320000;
  end = .;
  _end = .;
  _heap_start = ALIGN(0x1000);
  _pmem_start = pmem_base;
  _pmem_end = _pmem_start + LENGTH(ram);

  /DISCARD/ : {
    *(.note*)
    *(.comment*)
    *(.eh_frame*)
  }
}
