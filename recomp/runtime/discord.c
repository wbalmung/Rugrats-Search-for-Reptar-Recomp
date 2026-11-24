/* Discord Rich Presence over Discord's local IPC pipe (\\.\pipe\discord-ipc-N). No SDK, no DLL.
   Shows "Rugrats: Search for Reptar" + the level running now (hooks_level = database 0x800C5678) + time in the level.
   A background thread does all pipe I/O, so the game never waits on Discord. If Discord is not running it retries
   every 15 s. Uses the project's Discord application ID unless ini [discord] client_id or RUGRATS_DISCORD_ID gives another; the application's
   name is what Discord shows as "Playing ...". Turned off (opt.discord = 0) -> the status is cleared. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "hw.h"

extern int hooks_level;

#define GAME_NAME "Rugrats: Search for Reptar"

static const char *const lv_names[] = {
    "Menus", "Pickles' Home", "Grandpa's Teeth", "Incident on Aisle 7", "7 Voyages of Cynthia",
    "Visitors from Outer Space", "Toy Palace", "Reptar 2010", "Ice Cream Mountain", "The Mysterious Mr. Friend",
    "Chuckie's Glasses", "Circus Angelicus", "Let There Be Light", "Cookie Race", "The Return of Mr. Friend",
    "Mirrorland", "Training Level", "Gold Rush", "Egg Hunt", "Be Bad", "No More Cookies", "Touchdown Tommy",
    "Multiplayer Golf",
};

static HANDLE pipe = INVALID_HANDLE_VALUE;
static char client_id[32];
static char image[64];         /* optional Rich Presence art key ([discord] image) */
static int nonce;

static void level_text(int db, char *out, int n) {
    if (db <= 0) snprintf(out, n, "In the menus");
    else if (db < (int)(sizeof lv_names / sizeof lv_names[0])) snprintf(out, n, "%s", lv_names[db]);
    else if (db <= 31) {   /* golf dbs 23..31 are not in hole order: the game's hole index (80057d50) + 1 */
        static const char hole[9] = { 5, 3, 1, 7, 9, 2, 4, 6, 8 };
        snprintf(out, n, "Golf: hole %d", hole[db - 23]);
    }
    else snprintf(out, n, "Level %d", db);
}

static void pipe_close(void) {
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    pipe = INVALID_HANDLE_VALUE;
}

static int frame_send(uint32_t op, const char *json) {
    uint32_t len = (uint32_t)strlen(json);
    char buf[2048];
    DWORD done;
    if (len + 8 > sizeof buf) return 0;
    memcpy(buf, &op, 4);
    memcpy(buf + 4, &len, 4);
    memcpy(buf + 8, json, len);
    if (!WriteFile(pipe, buf, len + 8, &done, NULL) || done != len + 8) { pipe_close(); return 0; }
    return 1;
}

/* Read and drop whatever Discord sent (READY, command replies). Returns 0 if the pipe is gone or Discord closed it. */
static int drain(void) {
    char buf[1024];
    DWORD avail = 0, got;
    while (pipe != INVALID_HANDLE_VALUE) {
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) { pipe_close(); return 0; }
        if (!avail) return 1;
        if (!ReadFile(pipe, buf, avail < sizeof buf ? avail : sizeof buf, &got, NULL)) { pipe_close(); return 0; }
        if (got >= 8) {
            uint32_t op; memcpy(&op, buf, 4);
            if (op == 2) { rt_logf("discord: closed by Discord: %.*s\n", (int)(got - 8), buf + 8); pipe_close(); return 0; }
        }
    }
    return 0;
}

static int pipe_open(void) {
    char name[64], hs[128];
    for (int i = 0; i < 10; i++) {
        snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe != INVALID_HANDLE_VALUE) break;
    }
    if (pipe == INVALID_HANDLE_VALUE) return 0;
    snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", client_id);
    if (!frame_send(0, hs)) return 0;
    Sleep(500);                       /* let Discord answer the handshake (READY or an error close) */
    if (!drain()) return 0;
    rt_logf("discord: connected (%s)\n", name);
    return 1;
}

static int send_activity(int db, long long start) {
    char lv[64], js[1024];
    if (db == -100) {                 /* clear */
        snprintf(js, sizeof js, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu},\"nonce\":\"%d\"}",
                 GetCurrentProcessId(), ++nonce);
    } else {
        char art[160] = "";
        level_text(db, lv, sizeof lv);
        if (image[0]) snprintf(art, sizeof art, ",\"assets\":{\"large_image\":\"%s\",\"large_text\":\"%s\"}", image, GAME_NAME);
        snprintf(js, sizeof js,
                 "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{\"details\":\"%s\",\"state\":\"%s\","
                 "\"timestamps\":{\"start\":%lld}%s}},\"nonce\":\"%d\"}",
                 GetCurrentProcessId(), GAME_NAME, lv, start, art, ++nonce);
    }
    return frame_send(1, js);
}

static DWORD WINAPI discord_thread(void *arg) {
    (void)arg;
    int shown = -1000;                /* level shown now; -1000 = nothing, -100 = cleared */
    long long start = 0;
    DWORD next_try = 0;
    int last_db = -1000;
    for (;;) {
        int db = *(volatile int *)&hooks_level;
        if (db != last_db) { last_db = db; start = (long long)time(NULL); }
        int want = opt.discord ? db : -100;
        if (pipe == INVALID_HANDLE_VALUE) {
            if (want != -100 && (int)(GetTickCount() - next_try) >= 0) {
                if (pipe_open()) shown = -1000;
                else next_try = GetTickCount() + 15000;
            }
        } else if (drain() && want != shown) {
            if (send_activity(want, start)) {
                shown = want;
                char lv[64]; level_text(want, lv, sizeof lv);
                rt_logf("discord: status %s\n", want == -100 ? "cleared" : lv);
            }
        }
        if (pipe == INVALID_HANDLE_VALUE) shown = -1000;
        Sleep(1000);
    }
    return 0;
}

/* The project's own Discord application ("Rugrats: Search for Reptar"); [discord] client_id overrides it. */
#define DISCORD_DEFAULT_ID "1570819608427110562"

void discord_start(void) {
    const char *e = getenv("RUGRATS_DISCORD_ID");
    snprintf(client_id, sizeof client_id, "%s", e && *e ? e : opt.discord_id[0] ? opt.discord_id : DISCORD_DEFAULT_ID);
    for (char *p = client_id; *p; p++) if (*p < '0' || *p > '9') { client_id[0] = 0; break; }  /* digits only */
    snprintf(image, sizeof image, "%s", opt.discord_image);
    for (char *p = image; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 32) { image[0] = 0; break; }
    if (!client_id[0]) { rt_logf("discord: no client_id in [discord], Rich Presence off\n"); return; }
    HANDLE t = CreateThread(NULL, 0, discord_thread, NULL, 0, NULL);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
}
