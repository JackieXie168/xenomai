/*
 * Copyright (C) 2006 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */

#include <psos+/psos.h>

extern int __psos_muxid;

u_long tm_wkafter(u_long ticks)
{
	return XENOMAI_SKINCALL1(__psos_muxid, __psos_tm_wkafter, ticks);
}

u_long tm_cancel(u_long tmid)
{
	return XENOMAI_SKINCALL1(__psos_muxid, __psos_tm_cancel, tmid);
}

u_long tm_evafter(u_long ticks, u_long events, u_long *tmid_r)
{
	return XENOMAI_SKINCALL3(__psos_muxid, __psos_tm_evafter, ticks, events, tmid_r);
}

u_long tm_get(u_long *date_r, u_long *time_r, u_long *ticks_r)
{
	return XENOMAI_SKINCALL3(__psos_muxid, __psos_tm_get, date_r, time_r, ticks_r);
}

u_long tm_set(u_long date, u_long time, u_long ticks)
{
	return XENOMAI_SKINCALL3(__psos_muxid, __psos_tm_set, date, time, ticks);
}

u_long tm_evwhen(u_long date, u_long time, u_long ticks, u_long events, u_long *tmid_r)
{
	return XENOMAI_SKINCALL5(__psos_muxid, __psos_tm_evwhen, date, time, ticks, events, tmid_r);
}

u_long tm_wkwhen(u_long date, u_long time, u_long ticks)
{
	return XENOMAI_SKINCALL3(__psos_muxid, __psos_tm_wkwhen, date, time, ticks);
}

u_long tm_evevery(u_long ticks, u_long events, u_long *tmid_r)
{
	return XENOMAI_SKINCALL3(__psos_muxid, __psos_tm_evwhen, ticks, events, tmid_r);
}
