#include "recomp_types.h"
#include "keyboard_trace.h"
#include <stdio.h>

/* Read-only observations of the pinned XBE's own text-entry window. Never
 * print the profile name. The UI pool has 30 objects of size 0x220. */
void second_sight_keyboard_trace(uint32_t site, uint32_t object, uint32_t caller)
{
    static unsigned reports;
    static uint32_t last_object, last_state, last_actions, last_selection;
    uint32_t descriptor, state, actions, selection, port, physical = ~0u;
    uint32_t pressed = 0, held = 0;
    if (reports >= 160 || object < 0x3C3880u || object >= 0x3C7840u ||
        (object - 0x3C3880u) % 0x220u != 0) return;
    descriptor = MEM32(object + 4);
    if (descriptor != 0x31DA88u && descriptor != 0x31DA98u) return;
    state = MEM32(object + 0x1AC);
    actions = MEM32(object + 0xC0);
    selection = MEM32(object + 0x188);
    if (site == 0x53370u && object == last_object && state == last_state &&
        actions == last_actions && selection == last_selection) return;
    last_object = object; last_state = state;
    last_actions = actions; last_selection = selection;
    port = MEM32(object + 0x38);
    if (port < 4) {
        physical = MEM32(0x38DBA8u + port * 4);
        if (physical < 4) {
            held = MEM32(0x38DBB8u + physical * 0x3Cu + 0xCu);
            pressed = MEM32(0x38DBB8u + physical * 0x3Cu + 0x10u);
        }
    }
    fprintf(stderr, "[KEYBOARD] n=%u site=%08X caller=%08X obj=%08X parent=%08X "
            "vb=%u descriptor=%08X state=%u timer=%08X port=%u pad=%u "
            "pressed=%08X held=%08X actions=%08X other=%08X selected=%u "
            "accept=%08X cancel=%08X cursor=%u max=%u\n",
            ++reports, site, caller, object, MEM32(object + 0x2C),
            MEM32(0x458CDCu), descriptor, state, MEM32(object + 0x1B0),
            port, physical, pressed, held, actions, MEM32(object + 0xC4),
            selection, MEM32(object + 0x1C8), MEM32(object + 0x1CC),
            MEM32(object + 0x210), MEM32(object + 0x214));
}

