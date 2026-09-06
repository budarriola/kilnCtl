// safety_trip_decision.c -- see safety_trip_decision.h's doc comment for why
// this exists as its own tiny, dependency-free translation unit rather than
// living inline in safety_link.c.
#include "safety_trip_decision.h"

safety_trip_decision_t safety_trip_decide_event(safety_trip_decision_input_t in)
{
    safety_trip_decision_t out;
    out.is_new_event =
        !in.trip_event_ever_received_before || in.cached_trip_last_seq != in.incoming_trip_seq;
    out.fault_sources_valid =
        in.trip_event_ever_received_before && in.cached_trip_last_seq != in.incoming_trip_seq;
    return out;
}
