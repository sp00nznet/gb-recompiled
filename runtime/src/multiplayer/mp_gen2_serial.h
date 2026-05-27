/**
 * @file mp_gen2_serial.h
 * @brief Gen 2 Pokémon link-cable serial bridge over ENet.
 *
 * Wires the runtime's GBPlatformCallbacks.serial_exchange to a UDP/ENet
 * channel between two rom.exe instances. The game ROMs handle every
 * piece of game-level protocol (handshake, trade-block exchange, battle
 * coordination) on top of the raw byte exchange — this layer just
 * relays SB bytes both directions.
 *
 * Single-threaded. ENet is pumped synchronously from the game-loop main
 * thread (mp_gen2_pump) and during blocking serial_exchange waits.
 *
 * See sp00nznet/pokemon-gold/docs/LINK_CABLE_DESIGN.md for the wire
 * protocol and timing model.
 */

#ifndef MP_GEN2_SERIAL_H
#define MP_GEN2_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;

typedef enum {
    MP_GEN2_IDLE = 0,          /* Not initialised / disconnected */
    MP_GEN2_LISTENING,         /* Host waiting for a peer */
    MP_GEN2_CONNECTING,        /* Client mid-handshake */
    MP_GEN2_CONNECTED,         /* Linked; serial exchange operational */
    MP_GEN2_DISCONNECTED,      /* Peer dropped or rejected */
} MPGen2State;

/* Initialise ENet (idempotent). Call once at platform startup. */
bool mp_gen2_init(void);

/* Tear down any active connection and ENet host. Idempotent. */
void mp_gen2_shutdown(void);

/* Bind a listening ENet host on `port` and wait for a peer. Returns
 * false if the port is already in use. */
bool mp_gen2_host(uint16_t port);

/* Open an ENet host and initiate connection to host:port. Returns
 * false immediately on resolver/socket failure; otherwise transitions
 * through MP_GEN2_CONNECTING -> MP_GEN2_CONNECTED (or DISCONNECTED on
 * timeout) as mp_gen2_pump runs. */
bool mp_gen2_connect(const char* host, uint16_t port);

/* Disconnect any active peer and return to IDLE. */
void mp_gen2_disconnect(void);

/* Drive ENet — call frequently from the main loop. */
void mp_gen2_pump(void);

/* Current connection state. */
MPGen2State mp_gen2_get_state(void);

/* Human-readable state, suitable for an overlay or menu. */
const char* mp_gen2_state_string(void);

/* The function registered as GBPlatformCallbacks.serial_exchange.
 * Blocks the calling (game) thread up to timeout_ms waiting for the
 * partner's byte. Returns 0xFF if not connected, on timeout, or if
 * the peer drops mid-exchange (cable-unplugged semantics). */
uint8_t mp_gen2_serial_exchange(GBContext* ctx,
                                 uint8_t out_byte,
                                 uint8_t mode,
                                 uint32_t timeout_ms);

/* Transport self-test. Call after mp_gen2_host or mp_gen2_connect;
 * exchanges 256 known bytes with the partner and prints PASS / FAIL.
 * Does not need a game ROM. Returns true on PASS. */
bool mp_gen2_self_test(bool is_master, uint32_t connect_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* MP_GEN2_SERIAL_H */
