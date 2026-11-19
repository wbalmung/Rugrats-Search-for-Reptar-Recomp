/* Save states (quick save / quick load), see savestate.c.

   Each emulated device serializes itself in a X_state(StateIO *) function, one call per field:
       st_prefix(s, "cdrom");
       ST(s, c.mode);                 // a scalar or fixed array, stored under the name "cdrom.c.mode"
       ST_N(s, "sram", sram, size);   // a buffer with an explicit name
   The same function saves and loads (st_loading(s) tells which). Fields are found by name when loading, so
   adding, removing or reordering fields in a later build keeps old states loadable: unknown names are skipped,
   missing ones keep their current value. Never reuse a name for a field with a different meaning. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct StateIO StateIO;
void st_prefix(StateIO *s, const char *prefix);
void st_field(StateIO *s, const char *name, void *p, size_t n);
int st_loading(StateIO *s);
int st_has(StateIO *s, const char *name);   /* loading: the file holds <prefix>.<name> */
#define ST(s, x) st_field((s), #x, &(x), sizeof(x))
#define ST_N(s, name, p, n) st_field((s), (name), (p), (n))

/* device state functions (each in its own file) */
void hw_state(StateIO *s);
void gpu_state(StateIO *s);
void gte_state(StateIO *s);
void bios_state(StateIO *s);
void cd_state(StateIO *s);
void spu_state(StateIO *s);
void sio_state(StateIO *s);
void mdec_state(StateIO *s);
void hooks_state(StateIO *s);
void host_state(StateIO *s);          /* host_win32.c */
void host_state_done(int loaded);     /* host_win32.c: after a save (0) or load (1) */

/* host side */
void state_request(int save, int slot);  /* done at the next safe point (start of a game frame) */
int state_slot_exists(int slot);
void state_safe_point(void);             /* called at the start of the frame pacing hook */
void state_boot(void (*start)(void));    /* runs start(); a load later restarts from here */
extern char state_msg[128];              /* last result, shown by the host for a few seconds */
extern uint32_t state_msg_time;          /* GetTickCount() when state_msg was set */
