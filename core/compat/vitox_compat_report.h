// Runtime counters of the Vitox compat layer (for the core probe's log).
#pragma once

struct VitoxCompatReport {
    unsigned add_veh_calls;
    unsigned add_veh_ok;
    unsigned remove_veh_calls;
    bool add_veh_resolved;
    bool remove_veh_resolved;
};

extern "C" VitoxCompatReport vitox_compat_report();
