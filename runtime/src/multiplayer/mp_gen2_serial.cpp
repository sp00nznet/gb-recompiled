/**
 * @file mp_gen2_serial.cpp
 * @brief Gen 2 Pokémon link-cable serial bridge over ENet.
 *
 * See header for design notes and
 * sp00nznet/pokemon-gold/docs/LINK_CABLE_DESIGN.md for protocol details.
 */

#include "mp_gen2_serial.h"

#include "gbrt.h"
#include "enet/enet.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <deque>

/* ============================================================================
 * Wire format
 *
 * One opcode for now: SERIAL_BYTE. The packet is sent on ENet channel 0
 * (reliable, ordered). Both sides exchange one SERIAL_BYTE per byte
 * transfer — master initiates, slave replies. Sequence numbers help
 * catch protocol drift (e.g. one side missed a byte).
 * ========================================================================== */

#define MP_GEN2_DEFAULT_PORT 21385  /* la-mp uses 21384; stay out of its way. */
#define MP_GEN2_PROTOCOL_VERSION 1

enum MPGen2Op : uint8_t {
    MP_GEN2_OP_SERIAL_BYTE = 0x01,
    MP_GEN2_OP_HELLO       = 0x02,  /* sent on connect; carries protocol version */
};

#pragma pack(push, 1)
struct MPGen2HelloPacket {
    uint8_t  op;          /* = MP_GEN2_OP_HELLO */
    uint8_t  version;     /* = MP_GEN2_PROTOCOL_VERSION */
    uint16_t reserved;
};

struct MPGen2SerialPacket {
    uint8_t  op;          /* = MP_GEN2_OP_SERIAL_BYTE */
    uint8_t  out_byte;    /* the byte the sender shifted out */
    uint8_t  mode;        /* GB_SERIAL_MASTER or GB_SERIAL_SLAVE */
    uint8_t  pad;
    uint32_t seq;         /* little-endian sequence number */
};
#pragma pack(pop)

/* ============================================================================
 * State
 * ========================================================================== */

namespace {

struct InboxEntry {
    uint8_t out_byte;
    uint8_t mode;
    uint32_t seq;
};

bool         g_enet_initialised = false;
MPGen2State  g_state            = MP_GEN2_IDLE;
ENetHost*    g_host             = nullptr;
ENetPeer*    g_peer             = nullptr;
uint32_t     g_next_send_seq    = 0;
uint32_t     g_next_recv_seq    = 0;
std::deque<InboxEntry> g_inbox;

const char* state_str_impl() {
    switch (g_state) {
        case MP_GEN2_IDLE:         return "Idle";
        case MP_GEN2_LISTENING:    return "Listening (waiting for partner)";
        case MP_GEN2_CONNECTING:   return "Connecting...";
        case MP_GEN2_CONNECTED:    return "Connected";
        case MP_GEN2_DISCONNECTED: return "Disconnected";
    }
    return "?";
}

void clear_host() {
    if (g_peer) {
        enet_peer_reset(g_peer);
        g_peer = nullptr;
    }
    if (g_host) {
        enet_host_destroy(g_host);
        g_host = nullptr;
    }
    g_inbox.clear();
    g_next_send_seq = 0;
    g_next_recv_seq = 0;
}

void send_hello(ENetPeer* peer) {
    MPGen2HelloPacket pkt = { MP_GEN2_OP_HELLO, MP_GEN2_PROTOCOL_VERSION, 0 };
    ENetPacket* p = enet_packet_create(&pkt, sizeof(pkt), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, p);
}

void handle_packet(ENetPeer* /*from*/, ENetPacket* pkt) {
    if (pkt->dataLength < 1) return;
    uint8_t op = pkt->data[0];
    if (op == MP_GEN2_OP_HELLO && pkt->dataLength >= sizeof(MPGen2HelloPacket)) {
        const MPGen2HelloPacket* h = reinterpret_cast<MPGen2HelloPacket*>(pkt->data);
        if (h->version != MP_GEN2_PROTOCOL_VERSION) {
            fprintf(stderr, "[MP-GEN2] Protocol mismatch (peer=%u, ours=%u). Dropping.\n",
                    h->version, MP_GEN2_PROTOCOL_VERSION);
            mp_gen2_disconnect();
            return;
        }
        fprintf(stderr, "[MP-GEN2] Peer hello received (proto v%u)\n", h->version);
    } else if (op == MP_GEN2_OP_SERIAL_BYTE && pkt->dataLength >= sizeof(MPGen2SerialPacket)) {
        const MPGen2SerialPacket* s = reinterpret_cast<MPGen2SerialPacket*>(pkt->data);
        InboxEntry e;
        e.out_byte = s->out_byte;
        e.mode     = s->mode;
        e.seq      = s->seq;  /* host-endian; both ends are LE in practice */
        g_inbox.push_back(e);
    }
}

}  /* anonymous namespace */

/* ============================================================================
 * Public API
 * ========================================================================== */

extern "C" {

bool mp_gen2_init(void) {
    if (!g_enet_initialised) {
        if (enet_initialize() != 0) {
            fprintf(stderr, "[MP-GEN2] enet_initialize failed\n");
            return false;
        }
        g_enet_initialised = true;
        fprintf(stderr, "[MP-GEN2] ENet initialised (channel 21385)\n");
    }
    return true;
}

void mp_gen2_shutdown(void) {
    clear_host();
    if (g_enet_initialised) {
        enet_deinitialize();
        g_enet_initialised = false;
    }
    g_state = MP_GEN2_IDLE;
}

bool mp_gen2_host(uint16_t port) {
    if (!mp_gen2_init()) return false;
    clear_host();

    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port ? port : MP_GEN2_DEFAULT_PORT;
    g_host = enet_host_create(&addr, /*peers*/ 1, /*channels*/ 1, 0, 0);
    if (!g_host) {
        fprintf(stderr, "[MP-GEN2] Failed to bind port %u (in use?)\n", addr.port);
        g_state = MP_GEN2_DISCONNECTED;
        return false;
    }
    g_state = MP_GEN2_LISTENING;
    fprintf(stderr, "[MP-GEN2] Listening on port %u\n", addr.port);
    return true;
}

bool mp_gen2_connect(const char* host, uint16_t port) {
    if (!mp_gen2_init()) return false;
    clear_host();

    g_host = enet_host_create(nullptr, /*peers*/ 1, /*channels*/ 1, 0, 0);
    if (!g_host) {
        fprintf(stderr, "[MP-GEN2] Failed to create client host\n");
        g_state = MP_GEN2_DISCONNECTED;
        return false;
    }

    ENetAddress addr;
    if (enet_address_set_host(&addr, host) != 0) {
        fprintf(stderr, "[MP-GEN2] Failed to resolve %s\n", host);
        clear_host();
        g_state = MP_GEN2_DISCONNECTED;
        return false;
    }
    addr.port = port ? port : MP_GEN2_DEFAULT_PORT;

    g_peer = enet_host_connect(g_host, &addr, /*channels*/ 1, /*data*/ 0);
    if (!g_peer) {
        fprintf(stderr, "[MP-GEN2] enet_host_connect failed\n");
        clear_host();
        g_state = MP_GEN2_DISCONNECTED;
        return false;
    }
    g_state = MP_GEN2_CONNECTING;
    fprintf(stderr, "[MP-GEN2] Connecting to %s:%u\n", host, addr.port);
    return true;
}

void mp_gen2_disconnect(void) {
    if (g_peer) {
        enet_peer_disconnect(g_peer, 0);
        /* Drain a few events so ENet notices the disconnect. */
        for (int i = 0; i < 4; i++) {
            ENetEvent ev;
            if (enet_host_service(g_host, &ev, 50) > 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            }
        }
    }
    clear_host();
    g_state = MP_GEN2_IDLE;
}

void mp_gen2_pump(void) {
    if (!g_host) return;
    ENetEvent ev;
    while (enet_host_service(g_host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                if (g_state == MP_GEN2_LISTENING) {
                    g_peer = ev.peer;
                    g_state = MP_GEN2_CONNECTED;
                    send_hello(g_peer);
                    fprintf(stderr, "[MP-GEN2] Partner connected from %x:%u\n",
                            ev.peer->address.host, ev.peer->address.port);
                } else if (g_state == MP_GEN2_CONNECTING) {
                    g_state = MP_GEN2_CONNECTED;
                    send_hello(ev.peer);
                    fprintf(stderr, "[MP-GEN2] Connected\n");
                }
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                fprintf(stderr, "[MP-GEN2] Peer disconnected\n");
                g_peer = nullptr;
                g_state = MP_GEN2_DISCONNECTED;
                g_inbox.clear();
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                handle_packet(ev.peer, ev.packet);
                enet_packet_destroy(ev.packet);
                break;
            default: break;
        }
    }
}

MPGen2State mp_gen2_get_state(void) { return g_state; }
const char* mp_gen2_state_string(void) { return state_str_impl(); }

/* Send a SERIAL_BYTE packet (helper used by both master and slave paths). */
static bool send_serial_byte(uint8_t out_byte, uint8_t mode) {
    MPGen2SerialPacket pkt = {};
    pkt.op       = MP_GEN2_OP_SERIAL_BYTE;
    pkt.out_byte = out_byte;
    pkt.mode     = mode;
    pkt.seq      = g_next_send_seq++;
    ENetPacket* p = enet_packet_create(&pkt, sizeof(pkt), ENET_PACKET_FLAG_RELIABLE);
    if (!p || enet_peer_send(g_peer, 0, p) != 0) {
        fprintf(stderr, "[MP-GEN2] send failed\n");
        return false;
    }
    enet_host_flush(g_host);
    return true;
}

uint8_t mp_gen2_serial_exchange(GBContext* /*ctx*/,
                                 uint8_t out_byte,
                                 uint8_t mode,
                                 uint32_t timeout_ms) {
    /* Not connected -> behave like an unplugged cable. */
    if (g_state != MP_GEN2_CONNECTED || !g_peer) {
        mp_gen2_pump();
        if (g_state != MP_GEN2_CONNECTED) return 0xFF;
    }

    uint32_t start = SDL_GetTicks();

    /* Real link-cable semantics: only the *master* writes onto the wire.
     * Slave's SC=0x80 means "ready to receive whenever master drives the
     * clock" — it does NOT push a byte by itself. Emulating that here is
     * important because Pokemon Gen 2's Cable Club spends a long time
     * with both sides in slave mode polling for the partner's first
     * handshake byte; if slave sent eagerly on every poll we'd flood the
     * network and the inbox would fill with stale bytes. */
    if (mode == GB_SERIAL_MASTER) {
        if (!send_serial_byte(out_byte, GB_SERIAL_MASTER)) return 0xFF;

        /* Now wait for the slave's reply. */
        while (true) {
            mp_gen2_pump();
            if (g_state != MP_GEN2_CONNECTED) return 0xFF;
            for (auto it = g_inbox.begin(); it != g_inbox.end(); ++it) {
                if (it->mode == GB_SERIAL_SLAVE) {
                    uint8_t in = it->out_byte;
                    g_inbox.erase(it);
                    return in;
                }
            }
            if (SDL_GetTicks() - start >= timeout_ms) {
                fprintf(stderr, "[MP-GEN2] master timed out waiting for slave (%u ms)\n", timeout_ms);
                return 0xFF;
            }
            SDL_Delay(1);
        }
    }

    /* SLAVE: wait for master to push a byte, then reply with our own. */
    while (true) {
        mp_gen2_pump();
        if (g_state != MP_GEN2_CONNECTED) return 0xFF;
        for (auto it = g_inbox.begin(); it != g_inbox.end(); ++it) {
            if (it->mode == GB_SERIAL_MASTER) {
                uint8_t in = it->out_byte;
                g_inbox.erase(it);
                send_serial_byte(out_byte, GB_SERIAL_SLAVE);
                return in;
            }
        }
        if (SDL_GetTicks() - start >= timeout_ms) {
            /* Timeout in slave mode is normal/common — Pokemon polls
             * heavily before handshake. Don't spam the log. */
            return 0xFF;
        }
        SDL_Delay(1);
    }
}

/* ============================================================================
 * Self-test
 *
 * Exercises the transport end-to-end without requiring an actual game
 * ROM in the loop. Call after mp_gen2_host or mp_gen2_connect. One side
 * runs as master, the other as slave; together they validate that 256
 * known bytes cross the wire in both directions with no corruption,
 * loss, or reordering.
 *
 * Exits with PASS / FAIL printed to stderr; returns true on PASS.
 * ========================================================================== */

bool mp_gen2_self_test(bool is_master, uint32_t connect_timeout_ms) {
    fprintf(stderr, "[MP-TEST] Waiting up to %u ms for partner...\n", connect_timeout_ms);
    uint32_t t0 = SDL_GetTicks();
    while (mp_gen2_get_state() != MP_GEN2_CONNECTED) {
        mp_gen2_pump();
        if (mp_gen2_get_state() == MP_GEN2_DISCONNECTED) {
            fprintf(stderr, "[MP-TEST] DISCONNECTED before handshake. FAIL.\n");
            return false;
        }
        if (SDL_GetTicks() - t0 >= connect_timeout_ms) {
            fprintf(stderr, "[MP-TEST] Connect timeout. FAIL.\n");
            return false;
        }
        SDL_Delay(10);
    }
    fprintf(stderr, "[MP-TEST] Linked as %s. Exchanging 256 bytes...\n",
            is_master ? "MASTER" : "SLAVE");

    int errors = 0;
    for (int i = 0; i < 256; i++) {
        uint8_t out          = is_master ? (uint8_t)i : (uint8_t)(255 - i);
        uint8_t expected_in  = is_master ? (uint8_t)(255 - i) : (uint8_t)i;
        uint8_t in = mp_gen2_serial_exchange(nullptr, out,
                                              is_master ? GB_SERIAL_MASTER : GB_SERIAL_SLAVE,
                                              2000);
        if (in != expected_in) {
            fprintf(stderr, "[MP-TEST] Byte %d FAIL: sent 0x%02X, expected 0x%02X, got 0x%02X\n",
                    i, out, expected_in, in);
            errors++;
            if (errors >= 8) {
                fprintf(stderr, "[MP-TEST] Aborting after 8 mismatches.\n");
                break;
            }
        }
    }
    if (errors == 0) {
        fprintf(stderr, "[MP-TEST] PASS (256/256 bytes round-tripped)\n");
        return true;
    }
    fprintf(stderr, "[MP-TEST] FAIL (%d errors)\n", errors);
    return false;
}

}  /* extern "C" */
