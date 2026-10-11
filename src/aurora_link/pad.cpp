/**
 * @file src/aurora_link/pad.cpp
 * @brief Pad host: xlink pad-state over UDP -> ViGEm virtual Xbox 360 pad.
 *
 * Ported verbatim from the standalone xlink-host.exe so behaviour is unchanged;
 * the only edits are moving it into the aurora_link namespace and taking the
 * protocol header from this directory instead of the hand-built tree. Keeping the
 * logic identical matters -- this path is latency-critical and was tuned against a
 * real controller (see the XLINK_VERBOSE note in the receive loop).
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <ViGEm/Client.h>

extern "C" {
#include "xlink_protocol.h"
}

#include "aurora_link.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "setupapi.lib")

#define XLINK_IDLE_UNPLUG_MS 5000

namespace aurora_link {

static SOCKET g_sock = INVALID_SOCKET;
static sockaddr_in g_pi_addr;      /* last sender -- where rumble is sent back */
static bool g_pi_known = false;
static volatile LONG g_rumble_seq = 0;
static volatile LONG g_running = 1;

/* ViGEm rumble callback: forward motor levels back to the Pi. */
static void CALLBACK rumble_cb(PVIGEM_CLIENT, PVIGEM_TARGET,
                               UCHAR large, UCHAR small, UCHAR, LPVOID)
{
    if (!g_pi_known) return;
    struct {
        struct xlink_header h;
        struct xlink_rumble r;
    } pkt;
    pkt.h.magic = XLINK_MAGIC;
    pkt.h.version = XLINK_VERSION;
    pkt.h.type = XLINK_T_RUMBLE;
    pkt.h.length = sizeof(pkt.r);
    pkt.h.seq = (uint32_t)InterlockedIncrement(&g_rumble_seq);
    pkt.r.large_motor = large;
    pkt.r.small_motor = small;
    sendto(g_sock, (const char *)&pkt, sizeof(pkt), 0,
           (sockaddr *)&g_pi_addr, sizeof(g_pi_addr));
}

static BOOL WINAPI ctrl_handler(DWORD)
{
    InterlockedExchange(&g_running, 0);
    request_helper_stop();
    return TRUE; /* main loop notices within one recv timeout */
}

int run_pad_host() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(XLINK_PAD_PORT);
    if (bind(g_sock, (sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind :%d failed (%d)\n", XLINK_PAD_PORT, WSAGetLastError());
        return 1;
    }
    DWORD rcv_timeout = 1000;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&rcv_timeout, sizeof(rcv_timeout));

    PVIGEM_CLIENT client = vigem_alloc();
    if (!client) { fprintf(stderr, "vigem_alloc failed\n"); return 1; }
    VIGEM_ERROR verr = vigem_connect(client);
    if (!VIGEM_SUCCESS(verr)) {
        fprintf(stderr, "vigem_connect failed (0x%x) — is ViGEmBus installed?\n", verr);
        return 1;
    }
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    printf("xlink-host: listening on UDP :%d (pad plugs in on demand)\n",
           XLINK_PAD_PORT);
    fflush(stdout);

    PVIGEM_TARGET pad = nullptr; /* plugged in only while the controller talks */
    XUSB_REPORT report;
    XUSB_REPORT_INIT(&report);
    ULONGLONG last_rx = 0;

    auto plug = [&]() -> bool {
        if (pad) return true;
        pad = vigem_target_x360_alloc();
        VIGEM_ERROR e = vigem_target_add(client, pad);
        if (!VIGEM_SUCCESS(e)) {
            fprintf(stderr, "vigem_target_add failed (0x%x)\n", e);
            vigem_target_free(pad);
            pad = nullptr;
            return false;
        }
        vigem_target_x360_register_notification(client, pad, rumble_cb, nullptr);
        XUSB_REPORT_INIT(&report);
        printf("xlink-host: controller active — virtual pad plugged in\n");
        fflush(stdout);
        return true;
    };
    auto unplug = [&](const char *why) {
        if (!pad) return;
        vigem_target_x360_unregister_notification(pad);
        vigem_target_remove(client, pad);
        vigem_target_free(pad);
        pad = nullptr;
        printf("xlink-host: virtual pad unplugged (%s)\n", why);
        fflush(stdout);
    };

    while (g_running && !helper_stopping()) {
        uint8_t buf[128];
        sockaddr_in from{};
        int fromlen = sizeof(from);
        int n = recvfrom(g_sock, (char *)buf, sizeof(buf), 0,
                         (sockaddr *)&from, &fromlen);
        if (n < 0) {
            /* timeout (or transient error): unplug if the Pi went silent */
            if (pad && last_rx &&
                GetTickCount64() - last_rx > XLINK_IDLE_UNPLUG_MS)
                unplug("idle timeout");
            continue;
        }
        if (n < (int)sizeof(struct xlink_header)) continue;

        struct xlink_header *h = (struct xlink_header *)buf;
        if (h->magic != XLINK_MAGIC || h->version != XLINK_VERSION) continue;

        /* remember the Pi so rumble can be routed back to it */
        g_pi_addr = from;
        g_pi_known = true;
        last_rx = GetTickCount64();

        switch (h->type) {
        case XLINK_T_PAD_STATE:
            if (!plug()) break;
            if (n >= (int)(sizeof(*h) + sizeof(struct xlink_pad_state))) {
                struct xlink_pad_state *s =
                    (struct xlink_pad_state *)(buf + sizeof(*h));
                /* field order matches XUSB_REPORT exactly */
                report.wButtons = s->buttons;
                report.bLeftTrigger = s->left_trigger;
                report.bRightTrigger = s->right_trigger;
                report.sThumbLX = s->lx;
                report.sThumbLY = s->ly;
                report.sThumbRX = s->rx;
                report.sThumbRY = s->ry;
                vigem_target_x360_update(client, pad, report);

                /* Detailed per-frame log is opt-in via XLINK_VERBOSE=1 (off by
                 * default so the input hot path avoids a per-frame printf+fflush
                 * synchronous disk write). A lightweight heartbeat still writes at
                 * most ~1/s so the log keeps growing while input flows — inputgap-
                 * watch.ps1 detects a stream drop by watching this file stop
                 * growing, so it must never go fully silent during active input. */
                static const bool verbose = getenv("XLINK_VERBOSE") != nullptr;
                static uint16_t last_btns = 0;
                static uint32_t pkts = 0;
                static DWORD last_log_ms = 0;
                ++pkts;
                DWORD now_ms = GetTickCount();
                if (verbose && (s->buttons != last_btns || (pkts % 120) == 0)) {
                    printf("xlink-host: btns=0x%04x LT=%3u RT=%3u "
                           "L=(%6d,%6d) R=(%6d,%6d)\n",
                           s->buttons, s->left_trigger, s->right_trigger,
                           s->lx, s->ly, s->rx, s->ry);
                    fflush(stdout);
                    last_btns = s->buttons;
                    last_log_ms = now_ms;
                } else if (now_ms - last_log_ms >= 800) {
                    printf("xlink-host: rx %u\n", pkts);
                    fflush(stdout);
                    last_log_ms = now_ms;
                }
            }
            break;
        case XLINK_T_KEEPALIVE:
            plug(); /* controller connected but idle — keep/make it present */
            break;
        case XLINK_T_BYE:
            unplug("controller disconnected");
            break;
        case XLINK_T_HELLO: {
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            printf("xlink-host: session hello from %s\n", ip);
            fflush(stdout);
            break;
        }
        default:
            break;
        }
    }

    unplug("shutdown");
    vigem_disconnect(client);
    vigem_free(client);
    closesocket(g_sock);
    WSACleanup();
    printf("xlink-host: clean exit\n");
    return 0;
}

}  // namespace aurora_link
