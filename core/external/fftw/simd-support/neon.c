/*
 * Copyright (c) 2003, 2007-14 Matteo Frigo
 * Copyright (c) 2003, 2007-14 Massachusetts Institute of Technology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "kernel/ifftw.h"

#if HAVE_NEON

/* HiDef Radio (build 14): on 64-bit ARM, NEON is part of the architecture (every
   AArch64 processor has it), so no run-time probe is needed; and the probe below is
   32-bit ARM code (the word 0xf2000150 is the 32-bit encoding of "vand q0, q0, q0";
   executed as a 64-bit instruction it happens to be "ands x16, x10, #..." - harmless,
   but not what it says). Skip it on AArch64 and take the "assume present" branch. */
/* check for an environment where signals are known to work */
#if (defined(unix) || defined(linux)) && !defined(__aarch64__)
  # include <signal.h>
  # include <setjmp.h>

  static jmp_buf jb;

  static void sighandler(int x)
  {
       UNUSED(x);
       longjmp(jb, 1);
  }

  static int really_have_neon(void)
  {
       void (*oldsig)(int);
       oldsig = signal(SIGILL, sighandler);
       if (setjmp(jb)) {
	    signal(SIGILL, oldsig);
	    return 0;
       } else {
	    /* paranoia: encode the instruction in binary because the
	       assembler may not recognize it without -mfpu=neon */
	    /*asm volatile ("vand q0, q0, q0");*/
	    asm volatile (".long 0xf2000150");
	    signal(SIGILL, oldsig);
	    return 1;
       }
  }

  int X(have_simd_neon)(void)
  {
       static int init = 0, res;

       if (!init) {
	    res = really_have_neon();
	    init = 1;
       }
       return res;
  }


#else
/* don't know how to autodetect NEON; assume it is present */
  int X(have_simd_neon)(void)
  {
       return 1;
  }
#endif

#endif
