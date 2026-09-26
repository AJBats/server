// cardian_link_protocol.h -- every message of the Cardian Link, the TCP channel
// between the companion addon and the map server (RESEARCH.md §7).
//
// One definition, two readers. The map server compiles this file (through
// cardian_link_messages.h, which includes <cstdint> first and pairs each struct
// with its type number), and dev-publish.ps1 copies it into the addon, whose
// link.lua hands the text to LuaJIT's ffi.cdef. So it holds only plain C that
// both accept: no #include, #define or #if (LuaJIT rejects them), fixed-width
// integers, no bool, and no enum-typed fields (a compiler picks an enum's size;
// enum constants are fine). Packed and little-endian: the same bytes both sides.
//
// The rules every message follows:
//   - It starts with cl_header. Message CL_T_FOO is the struct cl_foo, and a
//     type number is never reused. The high byte is the area: 0x00 the link
//     itself, 0x01 a cardian's state, 0x02 items and gear, 0x03 gambits, 0x04
//     orders and control, 0x05 the pause and the server's other notices, 0x06
//     the party finder, 0x07 the conquest exchange.
//   - A request carries req != 0. Every answer echoes req with CL_F_REPLY; all
//     but the last also carry CL_F_MORE. The last answer is the request's own
//     message and carries the outcome in status. req 0 is one-way: nobody answers.
//   - A request and its last answer share one struct: the asker fills the
//     request's fields, the answer adds its own (bind's name, hello's version).
//     A request the link itself refuses (not bound, malformed, unknown) comes
//     back as itself, with CL_F_REPLY and the status.
//   - Characters are named by charid, entities within a zone by target index.
//   - Text fields are for people to read, never for code to parse. The sender
//     cuts text to fit, and a field always keeps its terminating zero.
//   - A message's size equals its struct's size, except CL_T_LEGACY_CD, whose
//     text follows the struct.
//   - Every constant is written with its value: the addon reads the numbers
//     from this text to name messages and outcomes in its logs.
#pragma once
#pragma pack(push, 1)

// The link's protocol number. Bump it whenever a message changes shape: hello
// carries it both ways, and a mismatch unloads the addon (no message is kept
// compatible, the user, 2026-09-14). 17: the party's orders (ORDERS and the
// messages that change them) and ENGAGE; 16: WALK, VIEW and the maneuver
// messages (their lines leave LEGACY_CD); 15: binary messages, this file; 14
// and earlier were newline text.
enum { CL_PROTOCOL = 17 };

// 'CDLK' as its bytes arrive: hello comes from a Cardian peer, not a stray connection
enum { CL_MAGIC = 0x4B4C4443 };

// Starts every message, both directions
typedef struct cl_header
{
    uint32_t size;   // bytes in the whole message, this header included
    uint16_t type;   // CL_T_*: which struct follows
    uint16_t flags;  // CL_F_*
    uint32_t req;    // the asker's request id, echoed by every answer; 0 = one-way
    uint16_t status; // CL_S_*: the outcome on a request's last answer, the reason on a notice; else 0
    uint16_t spare;  // 0: room for the header to grow
} cl_header;

enum
{
    CL_F_REPLY = 0x0001, // answers request req
    CL_F_MORE  = 0x0002, // another answer to req follows
};

// Outcomes. The server names them; the addon words them for the player.
enum
{
    CL_S_OK                = 0,

    // The link's own
    CL_S_UNKNOWN_TYPE      = 1, // the receiver has no such message
    CL_S_MALFORMED         = 2, // the wrong size, or a field out of range
    CL_S_HELLO_FIRST       = 3, // the first message must be hello
    CL_S_PROTOCOL_MISMATCH = 4, // the two sides were built from different versions of this file
    CL_S_NOT_BOUND         = 5, // bind first
    CL_S_BIND_STALE        = 6, // the bound character is gone or played elsewhere now: bind again
    CL_S_NO_SUCH_CHARACTER = 7, // bind: nobody by that charid is online
    CL_S_NOT_PLAYED        = 8, // bind: a character with no client of its own (a cardian)
    CL_S_ADDRESS_MISMATCH  = 9, // bind: that character's client is at another address

    // The game's
    CL_S_NO_SUCH_CARDIAN   = 0x0100, // not a cardian of yours that is out
    CL_S_OTHER_ZONE        = 0x0101, // she is in another zone
    CL_S_OUT_OF_REACH      = 0x0102, // she is beyond trading reach (pawn.TRADE_RANGE)
    CL_S_NO_ITEM           = 0x0103, // nothing in that slot
    CL_S_GIL_NOT_AN_ITEM   = 0x0104, // gil moves by its own message
    CL_S_ITEM_EQUIPPED     = 0x0105,
    CL_S_BAD_QUANTITY      = 0x0106,
    CL_S_ITEM_BUSY         = 0x0107,
    CL_S_ITEM_CANNOT_MOVE  = 0x0108,
    CL_S_NO_SPACE          = 0x0109, // she has no room for it
    CL_S_ITEM_SLIPPED_AWAY = 0x010A, // the stack changed while it moved
    CL_S_REFUSED           = 0x010B, // the game refused it; the map log says why

    // Maneuvers
    CL_S_NO_MANEUVER       = 0x0110, // she has no maneuver
    CL_S_ANOTHER_PLAYERS   = 0x0111, // another player drives her
    CL_S_MANEUVER_COMPOSED = 0x0112, // hers is composed already: cancel it first
    CL_S_KNOCKED_OUT       = 0x0113,
    CL_S_NOT_LOOKING       = 0x0114, // his camera is not on her
    CL_S_ONE_MANEUVER      = 0x0115, // one maneuver at a time: he drives another (the answer's other)
    CL_S_NOT_PAUSED        = 0x0116, // a move is a paused maneuver's order
    CL_S_NO_ROUTE          = 0x0117, // no route laid
    CL_S_ALREADY_RESTED    = 0x0118, // she is at that percent of HP and MP already

    // The party's orders
    CL_S_NO_STAKE          = 0x0120, // no camp stands to clear
    CL_S_NO_TARGET         = 0x0121, // nothing at that target index
    CL_S_RETREATING        = 0x0122, // the party is retreating: call it off first
    CL_S_NOT_A_MONSTER     = 0x0123, // a player or a cardian, not a monster
    CL_S_UNDERGROUND       = 0x0124, // the monster is out of reach underground
    CL_S_NO_CARDIANS_OUT   = 0x0125, // none of his cardians is out in his zone
};

// ---- 0x00xx: the link itself ----------------------------------------------

enum
{
    CL_T_HELLO     = 0x0001,
    CL_T_BIND      = 0x0002,
    CL_T_PING      = 0x0003,
    CL_T_BYE       = 0x0004,
    CL_T_POS       = 0x0005,
    CL_T_STATS     = 0x0006,
    CL_T_WHOAMI    = 0x0007,
    CL_T_UNBOUND   = 0x0008,
    CL_T_LEGACY_CD = 0x00FF,
};

// The first message each way: the addon asks, the server answers with its own
// protocol and build. On CL_S_PROTOCOL_MISMATCH the server closes and the addon
// unloads itself.
typedef struct cl_hello
{
    cl_header h;
    uint32_t  magic;       // CL_MAGIC
    uint32_t  protocol;    // CL_PROTOCOL as the sender was built
    char      version[48]; // the addon's version; answered with the server's git build
} cl_hello;

// Attach the link to a live character played from the peer's own address.
// Every later use checks it again, and CL_T_UNBOUND or CL_S_BIND_STALE says
// it is gone.
typedef struct cl_bind
{
    cl_header h;
    uint32_t  charid;
    char      name[16]; // answered: the character's name
} cl_bind;

// Either side asks after a quiet spell; the answer is the same message
typedef struct cl_ping
{
    cl_header h;
} cl_ping;

// One-way, from the addon: it is going, and the server closes
typedef struct cl_bye
{
    cl_header h;
} cl_bye;

// One-way, from a bound addon: its own character, streamed. Raw client memory
// (x, y, z in the client's axes, yaw in radians); the server swaps the axes.
typedef struct cl_pos
{
    cl_header h;
    float     x;
    float     y;
    float     z;
    float     yaw;
    uint8_t   moving; // 1 while the stick moves him
    uint8_t   spare[3];
} cl_pos;

// Development tools: the listener's counters since the map started
typedef struct cl_stats
{
    cl_header h;
    uint32_t  accepted;
    uint32_t  live;
    uint32_t  rejected;
    uint32_t  dropped;
    uint32_t  messagesIn;
    uint32_t  messagesOut;
    uint32_t  outDropped;
    uint32_t  posIn;
} cl_stats;

// Development tools: whom the server holds this link bound to
typedef struct cl_whoami
{
    cl_header h;
    uint32_t  charid; // answered
    uint16_t  zone;   // answered: the zone id
    uint16_t  spare;
    char      name[16]; // answered
} cl_whoami;

// One-way, from the server: a one-way message needed a bind this link does
// not hold, and status says why (CL_S_NOT_BOUND, CL_S_BIND_STALE). The addon
// forgets its bind and binds again.
typedef struct cl_unbound
{
    cl_header h;
} cl_unbound;

// Scaffolding while the text protocol is converted, one message at a time; it
// leaves before the conversion is merged. One-way, both ways: a line of the old
// protocol without its 'cd ' -- from the addon, a !cardian verb the server runs
// for the bound character; from the server, one of that command's replies. The
// text follows the struct, and h.size counts it.
typedef struct cl_legacy_cd
{
    cl_header h;
} cl_legacy_cd;

// ---- 0x01xx: a cardian's state --------------------------------------------

enum
{
    CL_T_INVENTORY = 0x0101,
};

enum { CL_ITEM_EQUIPPED = 0x01 };

typedef struct cl_item
{
    uint8_t  slot;
    uint8_t  flags; // CL_ITEM_*
    uint16_t id;
    uint32_t qty;
} cl_item;

// One of her containers as it stands: the inventory (loc 0) or a storage bag.
// An answer to whatever changed it.
typedef struct cl_inventory
{
    cl_header h;
    uint32_t  cardian;   // charid
    uint8_t   loc;
    uint8_t   size;      // slots the container has
    uint8_t   free;
    uint8_t   count;     // items in use
    cl_item   items[80]; // a container holds 80 at most
} cl_inventory;

// ---- 0x02xx: items and gear -----------------------------------------------

enum
{
    CL_T_GIVE = 0x0201,
};

// A stack from the player's inventory to hers, answered by her inventory
// (CL_F_MORE) and then the outcome
typedef struct cl_give
{
    cl_header h;
    uint32_t  cardian; // charid
    uint32_t  qty;
    uint8_t   slot;    // the player's inventory slot
    uint8_t   spare[3];
} cl_give;

// ---- 0x04xx: orders and control -------------------------------------------

enum
{
    CL_T_WALK      = 0x0401,
    CL_T_VIEW      = 0x0402,
    CL_T_MANEUVER     = 0x0403,
    CL_T_MANEUVERS    = 0x0404,
    CL_T_ORDERS       = 0x0405,
    CL_T_SET_STRATEGY = 0x0406,
    CL_T_SET_HUNT     = 0x0407,
    CL_T_RETREAT      = 0x0408,
    CL_T_STAKE        = 0x0409,
    CL_T_ENGAGE       = 0x040A,
};

// One-way, a stream like pos: direct control's walk order (pawn.h), walk her
// to a point in the server's axes (x, height, z), sent every frame the
// player's ring moves; off takes the order back. The point is slid along her
// zone's mesh from the last one toward the one asked, so it never leaves floor
// she can walk. The server says nothing unless the mesh moved the point or
// the order was refused (WALK_TAKEN).
typedef struct cl_walk
{
    cl_header h;
    uint32_t  cardian; // charid
    float     x;
    float     y;
    float     z;
    uint8_t   off;     // 1: take the walk order back
    uint8_t   spare[3];
} cl_walk;

// Direct control's view origin (pawn/view.h): the player looks through this
// cardian, so the world around her reaches his client even far from him.
// cardian 0 looks through nobody again.
typedef struct cl_view
{
    cl_header h;
    uint32_t  cardian; // charid, 0 = off
} cl_view;

enum
{
    CL_MV_BEGIN     = 0, // he takes the wheel: her maneuver begins, live
    CL_MV_OFF       = 1, // it ends
    CL_MV_MOVE      = 2, // a paused maneuver's order: walk the route the ring laid
    CL_MV_MOVE_WAIT = 3, // the same, then wait at its end
    CL_MV_REST      = 4, // rest until percent of HP and MP
};

// A maneuver (docs/maneuvers.md): the player drives one cardian live, or
// composes her order under a pause. Answered by the outcome alone; what the
// maneuver becomes is told by MANEUVER_STATE.
typedef struct cl_maneuver
{
    cl_header h;
    uint32_t  cardian;
    uint8_t   action;  // CL_MV_*
    uint8_t   percent; // CL_MV_REST: 1 to 100
    uint16_t  spare;
    uint32_t  other;   // answered with CL_S_ONE_MANEUVER: the cardian he drives already
} cl_maneuver;

// His maneuvers as they stand, for an addon that has just bound: each
// composed one comes as a MANEUVER_STATE answer (CL_F_MORE), then this, naming
// the one he drives live
typedef struct cl_maneuvers
{
    cl_header h;
    uint32_t  live; // answered: charid, 0 for none
} cl_maneuvers;

// The party's standing orders (M3.9, RESEARCH §12.16): the player's, for every
// cardian he commands. Asked for by the orders card, and sent as an answer
// (CL_F_MORE) ahead of the outcome of every change to them, refused or not.
typedef struct cl_orders
{
    cl_header h;
    uint8_t   strategy;   // answered: 0 Hold, 1 Pull (the addon words them)
    uint8_t   strategies; // answered: how many the server has
    uint8_t   retreat;    // answered: 1 while "on me" holds
    uint8_t   huntMin;    // answered: the /check, 0 Too Weak .. 7 Incredibly Tough
    uint8_t   huntMax;
    uint8_t   pull;       // answered: 0 nearest, 1 easiest, 2 toughest first
    uint8_t   aggressive; // answered: 1 when aggressive company is allowed
    uint8_t   links;      // answered: 1 when links are allowed
    uint8_t   staked;     // answered: 1 while a camp stands
    uint8_t   spare;
    uint16_t  stakeZone;  // answered: the camp's zone id
} cl_orders;

enum
{
    CL_STRATEGY_SET  = 0, // to strategy
    CL_STRATEGY_NEXT = 1, // the next one round
};

typedef struct cl_set_strategy
{
    cl_header h;
    uint8_t   mode;     // CL_STRATEGY_*
    uint8_t   strategy;
    uint16_t  spare;
} cl_set_strategy;

enum
{
    CL_HUNT_MIN        = 0, // value: the /check, 0..7
    CL_HUNT_MAX        = 1,
    CL_HUNT_PULL       = 2, // value: 0 nearest, 1 easiest, 2 toughest
    CL_HUNT_AGGRESSIVE = 3, // value: 0 or 1
    CL_HUNT_LINKS      = 4, // value: 0 or 1
};

typedef struct cl_set_hunt
{
    cl_header h;
    uint8_t   rule;  // CL_HUNT_*
    uint8_t   value;
    uint16_t  spare;
} cl_set_hunt;

enum
{
    CL_SWITCH_OFF    = 0,
    CL_SWITCH_ON     = 1,
    CL_SWITCH_TOGGLE = 2,
};

// "On me": every cardian disengages, engages nobody and avoids nothing, and
// hunting pauses, until it clears
typedef struct cl_retreat
{
    cl_header h;
    uint8_t   mode; // CL_SWITCH_*
    uint8_t   spare[3];
} cl_retreat;

enum
{
    CL_STAKE_SET    = 0, // set it, or move it, where he stands, facing his way
    CL_STAKE_CLEAR  = 1,
    CL_STAKE_TOGGLE = 2, // decided by the server, so two presses before the first answer still alternate
};

typedef struct cl_stake
{
    cl_header h;
    uint8_t   mode; // CL_STAKE_*
    uint8_t   spare[3];
} cl_stake;

// Every cardian of his in his zone fights his target. Answered by the outcome alone.
typedef struct cl_engage
{
    cl_header h;
    uint16_t  target; // a target index in his zone
    uint16_t  spare;
} cl_engage;

// ---- 0x05xx: the pause and the server's other notices ---------------------

enum
{
    CL_T_PAUSED         = 0x0501,
    CL_T_RESUMED        = 0x0502,
    CL_T_CALENDAR       = 0x0503,
    CL_T_MANEUVER_STATE = 0x0504,
    CL_T_WALK_TAKEN     = 0x0505,
};

// One-way, to the player steering her, when a walk order (WALK) was not taken
// as asked: the mesh moved the point (a wall, a ledge, the floor's height),
// so the ring follows it -- with the x and z asked, so the ring can apply the
// difference to wherever it has got to since; or the order was refused, and
// status says why.
typedef struct cl_walk_taken
{
    cl_header h;
    uint32_t  cardian; // charid
    float     x;       // the point as the mesh took it
    float     y;
    float     z;
    float     askedX;
    float     askedZ;
} cl_walk_taken;

enum
{
    CL_MS_ENDED    = 0, // hers ended: by his order leaving her, or any other door
    CL_MS_LIVE     = 1, // he drives it live, the camera on her
    CL_MS_COMPOSED = 2, // its order is given: it plays out without him
};

// One-way, to the player whose maneuver it is, at every change of it; and an
// answer to MANEUVERS. His camera follows the live one alone.
typedef struct cl_maneuver_state
{
    cl_header h;
    uint32_t  cardian;
    uint8_t   state; // CL_MS_*
    uint8_t   spare[3];
} cl_maneuver_state;

// One-way, to every bound addon as the simulation is held, and to an addon
// that binds while it is. gametime is Vana'diel's clock in seconds
// (vanadiel_timestamp), which stands still while held.
typedef struct cl_paused
{
    cl_header h;
    uint32_t  holder;         // charid of who took the hold; 0 for nobody in particular
    uint32_t  gametime;
    char      holderName[16]; // for the banner
} cl_paused;

// One-way, to every bound addon as the hold is let go
typedef struct cl_resumed
{
    cl_header h;
    uint32_t  gametime;
} cl_resumed;

// One-way, to an addon that binds into a running simulation: where the
// calendar stands (it runs behind real time by every pause so far)
typedef struct cl_calendar
{
    cl_header h;
    uint32_t  gametime;
} cl_calendar;

#pragma pack(pop)
