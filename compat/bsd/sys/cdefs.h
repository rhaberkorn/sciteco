/*
 * Copyright © 2004, 2005, 2006, 2009 Guillem Jover
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef LIBBSD_CDEFS_H
#define LIBBSD_CDEFS_H

#include <sys/cdefs.h>

#ifndef __dead2
# define __dead2
#endif

#ifndef __pure2
# define __pure2
#endif

/* Linux headers define a struct with a member names __unused.
 * Debian bugs: #522773 (linux), #522774 (libc).
 * Disable for now. */
#if 0
#ifndef __unused
# ifdef __GNUC__
#  define __unused __attribute__((unused))
# else
#  define __unused
# endif
#endif
#endif

#ifndef __printflike
# ifdef __GNUC__
#  define __printflike(x, y) __attribute((format(printf, (x), (y))))
# else
#  define __printflike(x, y)
# endif
#endif

#ifndef __bounded__
# define __bounded__(x, y, z)
#endif

#ifndef __RCSID
# define __RCSID(x)
#endif

#ifndef __FBSDID
# define __FBSDID(x)
#endif

#ifndef __RCSID
# define __RCSID(x)
#endif

#ifndef __RCSID_SOURCE
# define __RCSID_SOURCE(x)
#endif

#ifndef __SCCSID
# define __SCCSID(x)
#endif

#ifndef __COPYRIGHT
# define __COPYRIGHT(x)
#endif

#endif
