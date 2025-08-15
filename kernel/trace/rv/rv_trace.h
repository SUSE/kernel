/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM rv

#if !defined(_TRACE_RV_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RV_H

#include <linux/rv.h>
#include <linux/tracepoint.h>

#ifdef CONFIG_DA_MON_EVENTS_IMPLICIT
DECLARE_EVENT_CLASS(event_da_monitor,

	TP_PROTO(char *state, char *event, char *next_state, bool final_state),

	TP_ARGS(state, event, next_state, final_state),

	TP_STRUCT__entry(
		__string(	state,		state		)
		__string(	event,		event		)
		__string(	next_state,	next_state	)
		__field(	bool,		final_state	)
	),

	TP_fast_assign(
		__assign_str(state);
		__assign_str(event);
		__assign_str(next_state);
		__entry->final_state = final_state;
	),

	TP_printk("%s x %s -> %s%s",
		__get_str(state),
		__get_str(event),
		__get_str(next_state),
		__entry->final_state ? " (final)" : "")
);

DECLARE_EVENT_CLASS(error_da_monitor,

	TP_PROTO(char *state, char *event),

	TP_ARGS(state, event),

	TP_STRUCT__entry(
		__string(	state,	state	)
		__string(	event,	event	)
	),

	TP_fast_assign(
		__assign_str(state);
		__assign_str(event);
	),

	TP_printk("event %s not expected in the state %s",
		__get_str(event),
		__get_str(state))
);

#include <monitors/wip/wip_trace.h>
#include <monitors/tss/tss_trace.h>
#include <monitors/sco/sco_trace.h>
#include <monitors/scpd/scpd_trace.h>
#include <monitors/snep/snep_trace.h>
#include <monitors/sncid/sncid_trace.h>
// Add new monitors based on CONFIG_DA_MON_EVENTS_IMPLICIT here

#endif /* CONFIG_DA_MON_EVENTS_IMPLICIT */

#ifdef CONFIG_DA_MON_EVENTS_ID
DECLARE_EVENT_CLASS(event_da_monitor_id,

	TP_PROTO(int id, char *state, char *event, char *next_state, bool final_state),

	TP_ARGS(id, state, event, next_state, final_state),

	TP_STRUCT__entry(
		__field(	int,		id		)
		__string(	state,		state		)
		__string(	event,		event		)
		__string(	next_state,	next_state	)
		__field(	bool,		final_state	)
	),

	TP_fast_assign(
		__assign_str(state);
		__assign_str(event);
		__assign_str(next_state);
		__entry->id		= id;
		__entry->final_state	= final_state;
	),

	TP_printk("%d: %s x %s -> %s%s",
		__entry->id,
		__get_str(state),
		__get_str(event),
		__get_str(next_state),
		__entry->final_state ? " (final)" : "")
);

DECLARE_EVENT_CLASS(error_da_monitor_id,

	TP_PROTO(int id, char *state, char *event),

	TP_ARGS(id, state, event),

	TP_STRUCT__entry(
		__field(	int,	id	)
		__string(	state,	state	)
		__string(	event,	event	)
	),

	TP_fast_assign(
		__assign_str(state);
		__assign_str(event);
		__entry->id	= id;
	),

	TP_printk("%d: event %s not expected in the state %s",
		__entry->id,
		__get_str(event),
		__get_str(state))
);

#include <monitors/wwnr/wwnr_trace.h>
#include <monitors/snroc/snroc_trace.h>
// Add new monitors based on CONFIG_DA_MON_EVENTS_ID here

#endif /* CONFIG_DA_MON_EVENTS_ID */
#endif /* _TRACE_RV_H */

/* This part ust be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE rv_trace
#include <trace/define_trace.h>
