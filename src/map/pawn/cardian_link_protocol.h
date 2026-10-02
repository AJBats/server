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
//     the party finder, 0x07 the conquest exchange, 0x08 the Auction House.
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
//   - A message's size equals its struct's size.
//   - Every constant is written with its value: the addon reads the numbers
//     from this text to name messages and outcomes in its logs.
#pragma once
#pragma pack(push, 1)

// The link's protocol number. Bump it whenever a message changes shape: hello
// carries it both ways, and a mismatch unloads the addon (no message is kept
// compatible, the user, 2026-09-14). 32: ROLE_LOCKED, and a GAMBIT_ROW's on
// is the row as it runs; 31: a GAMBIT_ROW says whose it is, her
// own or lent by her party role (origin, lender); 30: a PARTY_ROLE carries the member's
// numbers and gear, and CL_ROLE_AUTO takes a choice back; 29: the party's roles,
// PARTY_ROLES, PARTY_ROLE and SET_PARTY_ROLE (RESEARCH §17); 28: the party finder's goals, GOALS
// and GOAL, and the conquest exchange, CP_SHOP, CP_ITEM and CP_BUY (the
// goals, cpshop and cpbuy lines leave, and LEGACY_CD with them: no text
// crosses the link any more); 27: NOTE, what came of a cardian's
// order after it was taken (the note line leaves LEGACY_CD for the server's
// pushes); 26: his cardians and the party finder,
// OWNED, SPAWN, DESPAWN, SHOUT, SHOUT_RESPONDER, PEEK, INVITE, CONTRACTS and
// END_CONTRACT (the owned, spawn, despawn, shout, peek, invite, contracts and
// endcontract lines leave LEGACY_CD); 25: gambits, GAMBITS, GAMBIT_ROW, the
// edits (GAMBIT_TOGGLE, _MOVE, _DELETE, _INSERT, _REPLACE, _MASTER) and the
// catalogue (GAMBIT_VOCAB, VOCAB_CONDITIONS, _STATUSES, _ACTIONS): rows as the
// gambit engine's own fields (the gambits, gb, g, gvocab, gv, gvc, gvs, gva,
// gvx, gtoggle, gmove, gdel, gins, gset and gmaster lines leave LEGACY_CD);
// 24: items and gear, TAKE, GIL, EQUIP,
// USE, DROP, SORT, MOVE and GIVE_USE (the take, givegil, takegil, wear, strip,
// equipset, use, drop, sort, move and giveuse lines leave LEGACY_CD; an
// AH_BID's notWorn is the equip's own outcome); 23: a cardian's state, ROSTER, MEMBER,
// SYNC, MEMBER_STATS, GEAR, BAGS, RECASTS, PROFILE, JOBS, SKILLS and an
// INVENTORY asked for (the list, sync, inv, bags, gear, recasts, profile,
// jobs, skills and mskills lines leave LEGACY_CD); 22: the Auction House, AH_SHELF,
// AH_HISTORY and AH_BID (the ahlist, ahhist and ahbid lines leave
// LEGACY_CD); 21: DO, QUEUE and QUEUES, and actions as
// typed fields (cl_action; the do, queues and q lines leave LEGACY_CD);
// 20: the Auction House screen's lines,
// on LEGACY_CD (ahlist <name> <shelf> -> ahl.b / ahl / ahl.e, ahhist ->
// ahh.b / ahh / ahh.e, ahbid -> ahb or err ahbid; list.b <count> <by a
// counter>; cd p's field 22 whether she stands by that counter, her charid
// now field 23; the text protocol's 16 to 18); 19: WAIT, RESCUE, HOMEPOINT,
// CANCEL and PAUSE (their lines leave LEGACY_CD); 18: the gambit review's lines, still on
// LEGACY_CD (the catalogue by side -- gvc <name> <page>:<range|-> <target>|
// <cond>:<arg|*|s>=<label>;..., no gvt -- every learnable action, gva keys
// ending ! not hers now, and the row state x-side; the text protocol's 15);
// 17: the party's orders (ORDERS and the messages that change them) and
// ENGAGE; 16: WALK, VIEW and the maneuver messages (their lines leave
// LEGACY_CD); 15: binary messages, this file; 14 and earlier were newline text.
enum { CL_PROTOCOL = 32 };

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
    CL_S_NO_SPACE          = 0x0109, // the inventory it was bound for has no room: hers, or his when he takes it back
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
    CL_S_NOT_IN_PARTY      = 0x0126, // nobody by that charid is in his party

    // One cardian's orders
    CL_S_NOT_KNOCKED_OUT   = 0x0130, // a home point is for a KO'd cardian
    CL_S_NOT_WHILE_PAUSED  = 0x0131, // a held simulation moves nobody
    CL_S_TOO_FAR           = 0x0132, // beyond the rescue's reach: the answer's away and range
    CL_S_COOLING_DOWN      = 0x0133, // a cooldown runs: the answer says how long (RESCUE's cooldownLeft, SHOUT's waitMs)
    CL_S_NOTHING_QUEUED    = 0x0134, // no command waits to be taken back
    CL_S_UNREACHED         = 0x0135, // she could not get in reach of the order's target
    CL_S_CANNOT_RECOVER    = 0x0136, // resting, she cannot recover right now (a poison, an avatar out)

    // The pause button
    CL_S_PAUSE_OFF         = 0x0140, // the pause is switched off on this server
    CL_S_LOGGING_OUT       = 0x0141, // nobody pauses on his way out of the game
    CL_S_SYNTHESIZING      = 0x0142, // nor in the middle of a synthesis
    CL_S_FISHING           = 0x0143, // nor with a line in the water
    CL_S_PAUSED_BY_OTHER   = 0x0144, // another player holds it, and only its holder resumes

    // The command window's orders (DO), and using an item
    CL_S_TOO_SOON          = 0x0150, // it could not start within the order's grace: the answer's wait
    CL_S_NOT_CARRIED       = 0x0151, // she carries none of that item
    CL_S_NOT_FIGHTING      = 0x0152, // a disengage with no fight to leave
    CL_S_NOT_MANAGED       = 0x0153, // her items are hers to use only when she is yours
    CL_S_CANNOT_NOW        = 0x0154, // the game would not start it now
    CL_S_ON_RECAST         = 0x0155, // its recast runs (an order within its grace waits it out instead)
    CL_S_STANDING_UP       = 0x0156, // she is getting up from a rest (an order waits instead)
    CL_S_ITEM_UNUSABLE     = 0x0157, // not an item anyone uses
    CL_S_INVENTORY_ONLY    = 0x0158, // an item is used or dropped from the inventory only
    CL_S_NO_SUCH_BAG       = 0x0159,

    // The Auction House (a full inventory is CL_S_NO_SPACE)
    CL_S_NOT_BY_COUNTER    = 0x0160, // the player stands by no auction counter
    CL_S_TOO_FAR_TO_SHOP   = 0x0161, // she does not stand by the counter he stands at
    CL_S_NO_SUCH_ITEM      = 0x0162,
    CL_S_NOT_IN_STACKS     = 0x0163, // that item does not come in stacks
    CL_S_BAD_PRICE         = 0x0164, // a bid is 1 to 999,999,999 gil
    CL_S_IN_EVENT          = 0x0165, // not during an event, hers or his
    CL_S_IN_JAIL           = 0x0166,
    CL_S_NO_AUCTION_HOUSE  = 0x0167, // her zone has none
    CL_S_WARDROBE_GEAR     = 0x0168, // only gear goes in a wardrobe
    CL_S_WORN_FROM_BAG     = 0x0169, // gear is worn from the inventory or a wardrobe only
    CL_S_CANNOT_WEAR       = 0x016A, // that cannot be worn there, by her
    CL_S_BAG_FULL          = 0x016B, // no room in that bag
    CL_S_NOT_ENOUGH_GIL    = 0x016C, // for a cardian: hers and his purse together
    CL_S_RARE_OWNED        = 0x016D, // it is Rare, and one is already owned
    CL_S_NOTHING_AT_PRICE  = 0x016E, // nothing listed at that price or less
    CL_S_PURCHASE_FAILED   = 0x016F, // a listing was there and the purchase still failed: try again

    // Her items and gear (no room where a stack was bound: CL_S_NO_SPACE an
    // inventory, CL_S_BAG_FULL a bag)
    CL_S_GIL_FULL          = 0x0170, // the receiving purse cannot hold that much
    CL_S_NOT_EQUIPMENT     = 0x0171, // that container slot holds nothing to wear
    CL_S_CANNOT_REMOVE     = 0x0172, // the game kept the piece on
    CL_S_USING_ITEM        = 0x0173, // she is using an item: her worn pieces stay where they are meanwhile
    CL_S_VIA_INVENTORY     = 0x0174, // a stack moves between her inventory and a bag, never bag to bag

    // Her gambits
    CL_S_NO_SUCH_ROW       = 0x0180,
    CL_S_ATTACK_ALONE      = 0x0181, // Attack goes alone on its row
    CL_S_ATTACK_ON_CLOCK   = 0x0182, // an Attack row cannot wait on a timer or a chance
    CL_S_ROLE_LOCKED       = 0x0183, // the row is her party role's, pinned while she holds the role: its content edited nowhere

    // His cardians, and the party finder
    CL_S_CANNOT_SPAWN      = 0x0190, // not his, online already, out already, or pawns switched off
    CL_S_NOT_OUT           = 0x0191, // a despawn for one of his who is not out
    CL_S_PARTY_FULL        = 0x0192,
    CL_S_NOT_LEADER        = 0x0193, // the party's leader invites
    CL_S_NO_MISSION        = 0x0194, // a mission shout from a log he holds no mission in
    CL_S_NOT_IN_SHOUT      = 0x0195, // nobody by that charid in his shout, or held by his contract
    CL_S_NOT_ADVENTURER    = 0x0196, // not one of the world's adventurers
    CL_S_CONTRACTED        = 0x0197, // under contract with another player
    CL_S_SHOUT_FADED       = 0x0198, // his shout has lapsed (a shout lives ten minutes): shout again
    CL_S_NOT_A_YES         = 0x0199, // she did not answer his shout for that
    CL_S_NOT_IN_CITY       = 0x019A, // she is not in his city
    CL_S_DECLINES          = 0x019B, // she says no, whatever she said before: the answer's line is hers
    CL_S_AWAY              = 0x019C, // she is away from the world
    CL_S_CANNOT_STAND      = 0x019D, // she is faded and cannot stand just now
    CL_S_IN_A_PARTY        = 0x019E,
    CL_S_INVITE_PENDING    = 0x019F, // she already has an invite
    CL_S_NO_CONTRACT       = 0x01A0, // she holds no contract with him

    // The conquest exchange (no room in her inventory: CL_S_NO_SPACE)
    CL_S_NO_GUARD          = 0x01B0, // the player stands by no gate guard that sells
    CL_S_NOT_SOLD          = 0x01B1, // this guard does not sell that
    CL_S_NOT_BY_PROXY      = 0x01B2, // the experience rings are sold to a player in person
    CL_S_OUTRANKED         = 0x01B3, // another nation's guard sells only to a nation that outranks his
    CL_S_FOREIGN_PLACE     = 0x01B4, // another nation's guard never sells what a conquest place buys
    CL_S_NATION_PLACE      = 0x01B5, // her nation's conquest place is below the item's: the answer's have and need
    CL_S_TOO_FEW_POINTS    = 0x01B6, // her conquest points fall short: the answer's have and need
    CL_S_RANK_TOO_LOW      = 0x01B7, // her rank is below the item's: the answer's have and need
    CL_S_GUARD_REFUSED     = 0x01B8, // the guard's own rules refused it, for none of the reasons above
};

// An action, as the command window gives one and a queue line shows it: fields,
// never a text anyone parses. Kinds 1 to 4 are the gambit catalogue's, with its
// mode (2: the one id names; the ranged attack's is 0).
enum
{
    CL_AK_NONE        = 0,  // no action: a queue line with nothing waiting
    CL_AK_RANGED      = 1,
    CL_AK_MAGIC       = 2,  // id: the spell
    CL_AK_ABILITY     = 3,  // id: the job ability
    CL_AK_WEAPONSKILL = 4,  // id: the weapon skill
    CL_AK_ITEM        = 5,  // id: the item, used on herself
    CL_AK_ATTACK      = 6,  // the command window's Attack: fight the target
    CL_AK_DISENGAGE   = 7,
    CL_AK_MOVE        = 8,  // a paused maneuver's order: walk the route the ring laid
    CL_AK_MOVE_WAIT   = 9,  // the same, then wait at its end
    CL_AK_REST        = 10, // id: rest until this percent of HP and MP
    CL_AK_CLIENT      = 11, // a player's own command from his client: id the action menu's (packet 0x01A's action id)
    CL_AK_HEAL        = 12, // a player's own /heal
};

typedef struct cl_action
{
    uint8_t  kind; // CL_AK_*
    uint8_t  mode;
    uint16_t id;
} cl_action;

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
    // 0x00FF was LEGACY_CD, which carried the text protocol's lines (15 to 27)
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

// ---- 0x01xx: a cardian's state --------------------------------------------
//
// What the menu shows of a cardian he commands. Each is asked for, and the
// ones a change moves (MEMBER_STATS, GEAR, INVENTORY, BAGS) are also sent by
// themselves, one-way, as the change is made.

enum
{
    CL_T_INVENTORY    = 0x0101,
    CL_T_ROSTER       = 0x0102,
    CL_T_MEMBER       = 0x0103,
    CL_T_SYNC         = 0x0104,
    CL_T_MEMBER_STATS = 0x0105,
    CL_T_GEAR         = 0x0106,
    CL_T_BAGS         = 0x0107,
    CL_T_RECASTS      = 0x0108,
    CL_T_PROFILE      = 0x0109,
    CL_T_JOBS         = 0x010A,
    CL_T_SKILLS       = 0x010B,
    CL_T_OWNED        = 0x010C,
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
// Asked for (a cardian of his to manage, cardian and loc), and an answer to
// whatever changed it.
typedef struct cl_inventory
{
    cl_header h;
    uint32_t  cardian;   // charid
    uint8_t   loc;
    uint8_t   size;      // answered: slots the container has
    uint8_t   free;      // answered
    uint8_t   count;     // answered: items in use
    cl_item   items[80]; // answered: a container holds 80 at most
} cl_inventory;

// His party's roster: each cardian he commands and who is in a zone comes as
// a MEMBER answer (CL_F_MORE), by name, then this, with what the player himself
// stands by
typedef struct cl_roster
{
    cl_header h;
    uint8_t   count;     // answered: members sent
    uint8_t   byCounter; // answered: 1 while he stands by an auction counter
    uint16_t  spare;
} cl_roster;

enum
{
    CL_MEMBER_WAITING    = 0x01, // holding her ground, ordered or left behind by magic
    CL_MEMBER_OWNED      = 0x02, // his to manage; else a wild cardian in his party, orders only
    CL_MEMBER_BY_COUNTER = 0x04, // she stands by the auction counter he stands at
    CL_MEMBER_BY_GUARD   = 0x08, // a gate guard stands within his reach: the conquest exchange sells to her
};

// One cardian as the roster shows her: an answer to ROSTER and SYNC
typedef struct cl_member
{
    cl_header h;
    uint32_t  cardian;      // charid
    char      name[16];
    uint8_t   mainJob;
    uint8_t   mainLevel;
    uint8_t   subJob;
    uint8_t   subLevel;
    uint16_t  hp;
    uint16_t  maxHp;
    uint16_t  mp;
    uint16_t  maxMp;
    uint16_t  tp;
    uint16_t  zone;         // the zone id
    uint32_t  exp;          // on her main job
    uint32_t  tnl;          // what the next level costs
    uint8_t   flags;        // CL_MEMBER_*
    uint8_t   spare[3];
    char      zoneName[32]; // where she is, for people
} cl_member;

// Everything the equipment screen shows of one cardian: MEMBER, MEMBER_STATS,
// GEAR and, when she is his to manage, her INVENTORY, as answers (CL_F_MORE),
// then this
typedef struct cl_sync
{
    cl_header h;
    uint32_t  cardian; // charid
} cl_sync;

// The status pane of the equipment screen: the seven base stats, each its
// total and the part of it gear and effects give; attack, defence; her gil
// (0 unless she is his to manage)
typedef struct cl_member_stats
{
    cl_header h;
    uint32_t  cardian;  // charid
    int16_t   total[7]; // STR, DEX, VIT, AGI, INT, MND, CHR
    int16_t   bonus[7];
    uint16_t  attack;
    uint16_t  defence;
    uint32_t  gil;
} cl_member_stats;

// A piece she wears: its item, and the container and slot it is worn from
// (the inventory, or a wardrobe); item 0 for an empty equipment slot
typedef struct cl_worn
{
    uint16_t item;
    uint8_t  bag;
    uint8_t  slot;
} cl_worn;

typedef struct cl_gear
{
    cl_header h;
    uint32_t  cardian;  // charid
    cl_worn   worn[16]; // by equipment slot, main hand to back
} cl_gear;

// One of her storage bags: the container, its slots and those in use
typedef struct cl_bag
{
    uint8_t loc;
    uint8_t size;
    uint8_t used;
    uint8_t spare;
} cl_bag;

// Her storage bags, in the order the menu cycles them (a cardian of his to
// manage)
typedef struct cl_bags
{
    cl_header h;
    uint32_t  cardian;  // charid
    uint8_t   count;    // answered
    uint8_t   spare[3];
    cl_bag    bags[16]; // answered
} cl_bags;

// An action still on recast, and the seconds left on it
typedef struct cl_recast
{
    cl_action action;
    float     seconds;
} cl_recast;

// What she cannot do yet: every spell and ability of hers still on recast,
// by the action the command window lists it as; the rest are ready. Answered
// in parts, each with its share (CL_F_MORE), the last with the outcome
typedef struct cl_recasts
{
    cl_header h;
    uint32_t  cardian;       // charid
    uint8_t   count;         // answered: recasts in this part
    uint8_t   spare[3];
    cl_recast recasts[64];   // answered
} cl_recasts;

// What the client's own Profile screen shows, for her
typedef struct cl_profile
{
    cl_header h;
    uint32_t  cardian;      // charid
    uint16_t  title;        // answered
    uint8_t   nation;       // answered
    uint8_t   race;         // answered
    uint8_t   rank;         // answered: her rank in her nation
    uint8_t   spare;
    uint16_t  homeZone;     // answered: her home point's zone
    uint32_t  rankPoints;   // answered
    char      homeName[32]; // answered: her home point's zone, for people
} cl_profile;

// Her level in every job, by job id (0 for a job she has none in)
typedef struct cl_jobs
{
    cl_header h;
    uint32_t  cardian;    // charid
    uint8_t   levels[24]; // answered
} cl_jobs;

enum
{
    CL_SKILLS_COMBAT = 0,
    CL_SKILLS_MAGIC  = 1,
};

// A skill her jobs can raise: its level as the client's skill pages show it
// (whole levels), and its cap at her level (the higher of main and support job)
typedef struct cl_skill
{
    uint16_t skill;
    uint16_t level;
    uint16_t cap;
    uint16_t spare;
} cl_skill;

typedef struct cl_skills
{
    cl_header h;
    uint32_t  cardian;    // charid
    uint8_t   kind;       // CL_SKILLS_*
    uint8_t   count;      // answered
    uint16_t  spare;
    cl_skill  skills[32]; // answered
} cl_skills;

// One character he could spawn as a cardian: his account's own alts and the
// cardians it owns, never the one he plays
typedef struct cl_owned_cardian
{
    uint32_t cardian;  // charid
    uint8_t  out;      // 1 while she is out as a cardian
    uint8_t  spare[3];
    char     name[16];
} cl_owned_cardian;

// Every character he could spawn, by name: answered in parts, each with its
// share (CL_F_MORE), the last with the outcome
typedef struct cl_owned
{
    cl_header        h;
    uint8_t          count;        // answered: in this part
    uint8_t          spare[3];
    cl_owned_cardian cardians[32]; // answered
} cl_owned;

// ---- 0x02xx: items and gear -----------------------------------------------
//
// Changes to the items and gear of a cardian of his to manage. A change that
// moved something -- all it was asked, or part of it before a refusal -- is
// answered by what it moved (CL_F_MORE) ahead of its outcome: her containers,
// bags, gear and status pane as they now stand, each message naming its own.
// One refused before it moved anything is answered by the outcome alone;
// EQUIP, a loadout of many slots, answers with her state whenever it was tried.

enum
{
    CL_T_GIVE     = 0x0201,
    CL_T_TAKE     = 0x0202,
    CL_T_GIL      = 0x0203,
    CL_T_EQUIP    = 0x0204,
    CL_T_USE      = 0x0205,
    CL_T_DROP     = 0x0206,
    CL_T_SORT     = 0x0207,
    CL_T_MOVE     = 0x0208,
    CL_T_GIVE_USE = 0x0209,
};

// A stack from the player's inventory to hers, answered by her inventory
typedef struct cl_give
{
    cl_header h;
    uint32_t  cardian; // charid
    uint32_t  qty;
    uint8_t   slot;    // the player's inventory slot
    uint8_t   spare[3];
} cl_give;

// A stack from her inventory back to his, answered by her inventory
typedef struct cl_take
{
    cl_header h;
    uint32_t  cardian; // charid
    uint32_t  qty;
    uint8_t   slot;    // her inventory slot
    uint8_t   spare[3];
} cl_take;

// Gil as the trade window's gil line moves it, to her or back from her,
// answered by her status pane (her gil rides it)
typedef struct cl_gil
{
    cl_header h;
    uint32_t  cardian; // charid
    uint32_t  amount;
    uint8_t   toHer;   // 1: his gil to her; 0: hers back to him
    uint8_t   spare[3];
} cl_gil;

// One equipment slot of a loadout: the piece to wear there, by the container
// (the inventory or a wardrobe) and slot it sits in; slot 0 leaves it bare
typedef struct cl_equip_slot
{
    uint8_t equipSlot; // 0 main hand .. 15 back
    uint8_t bag;
    uint8_t slot;
    uint8_t spare;
} cl_equip_slot;

// A loadout in one pass, as the equipment screen drafts it: the slots left
// bare first, freeing hands and slots, then each piece put on from the main
// hand up, so the rules of the second hand see the new first. A slot that
// does not take leaves the rest to go on. Once tried, answered by her status
// pane, gear, inventory and every wardrobe the loadout touched, then the
// outcome: CL_S_OK when every slot took, else the first refusal, and results
// holds each slot's own.
typedef struct cl_equip
{
    cl_header     h;
    uint32_t      cardian;     // charid
    uint8_t       count;       // slots used
    uint8_t       spare[3];
    cl_equip_slot slots[16];
    uint16_t      results[16]; // answered: each slot's outcome (CL_S_*), in the order asked
} cl_equip;

// She uses an item on herself, as the game's own item use. Answered by the
// outcome alone: the stack thins when the use completes, and a later sync
// shows it.
typedef struct cl_use
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   bag;     // the inventory; any other is refused CL_S_INVENTORY_ONLY
    uint8_t   slot;
    uint8_t   spare[2];
} cl_use;

// Part of one of her stacks thrown away, answered by the container and her bags
typedef struct cl_drop
{
    cl_header h;
    uint32_t  cardian; // charid
    uint32_t  qty;
    uint8_t   bag;     // the inventory; any other is refused CL_S_INVENTORY_ONLY
    uint8_t   slot;
    uint8_t   spare[2];
} cl_drop;

// One of her containers merged and put in order, answered by the container,
// her bags and her gear (a worn piece may sit in a new slot)
typedef struct cl_sort
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   bag;
    uint8_t   spare[3];
} cl_sort;

// A stack between her inventory and one of her bags, either way; a worn piece
// moves whole, between the inventory and a wardrobe, and stays worn. Answered
// by both containers, her bags and her gear.
typedef struct cl_move
{
    cl_header h;
    uint32_t  cardian; // charid
    uint32_t  qty;
    uint8_t   from;    // the container it sits in
    uint8_t   slot;
    uint8_t   to;
    uint8_t   spare;
} cl_move;

// The scroll's way in one action: a stack from his inventory to hers, which
// she uses at once. Refused whole while the game is paused. Answered by her
// inventory once the stack is hers, then the outcome of the use.
typedef struct cl_give_use
{
    cl_header h;
    uint32_t  cardian; // charid
    uint32_t  qty;
    uint8_t   slot;    // the player's inventory slot
    uint8_t   given;   // answered: 1 once the stack is hers, so a refusal is the use's
    uint8_t   spare[2];
} cl_give_use;

// ---- 0x03xx: gambits -------------------------------------------------------
//
// A cardian's gambit rows (M3.85, the gambit editor) and the pickers'
// catalogue, for a cardian he commands. A row crosses as the gambit
// engine's own fields; its label is for people. Every edit is answered by
// her rows as they now stand -- each a GAMBIT_ROW, then GAMBITS (CL_F_MORE)
// -- and then its outcome, refused or not, so the editor never keeps a guess.

enum
{
    CL_T_GAMBITS          = 0x0301,
    CL_T_GAMBIT_ROW       = 0x0302,
    CL_T_GAMBIT_TOGGLE    = 0x0303,
    CL_T_GAMBIT_MOVE      = 0x0304,
    CL_T_GAMBIT_DELETE    = 0x0305,
    CL_T_GAMBIT_INSERT    = 0x0306,
    CL_T_GAMBIT_REPLACE   = 0x0307,
    CL_T_GAMBIT_MASTER    = 0x0308,
    CL_T_GAMBIT_VOCAB     = 0x0309,
    CL_T_VOCAB_CONDITIONS = 0x030A,
    CL_T_VOCAB_STATUSES   = 0x030B,
    CL_T_VOCAB_ACTIONS    = 0x030C,
};

// One condition of a row: the gambit engine's condition and its argument, in
// the group it belongs to (a row's conditions are listed group by group)
typedef struct cl_gambit_condition
{
    uint16_t condition;
    uint8_t  group;
    uint8_t  spare;
    uint32_t arg;
} cl_gambit_condition;

// One action of a row: the gambit engine's reaction, how it selects, and the
// argument (the spell, ability, weapon skill, family or behaviour)
typedef struct cl_gambit_action
{
    uint16_t reaction;
    uint16_t select;
    uint32_t arg;
} cl_gambit_action;

// A row as the gambit engine holds it: when its target meets every group of
// conditions -- all of a group, or any of it where orGroups says so -- it
// takes its first action it can
typedef struct cl_gambit
{
    uint16_t            target;         // the target selector
    uint16_t            retry;          // seconds before it fires again
    uint8_t             conditionCount;
    uint8_t             actionCount;
    uint8_t             orGroups;       // bit g: group g is met by any one of its conditions
    uint8_t             spare;
    cl_gambit_condition conditions[16];
    cl_gambit_action    actions[8];
} cl_gambit;

// A row's meaning (RESEARCH 17.13): a plain row is an order; a row carrying
// the tactician's mark (the Tactician's choice condition) is a tool its
// judgement uses, or struck out. 1 and 5 were the tactician line's, retired
// with it and never sent
enum
{
    CL_GS_ORDER        = 0, // an order
    CL_GS_TOOL         = 2, // marked: something her tactician may use
    CL_GS_NO_JUDGEMENT = 3, // marked, and nothing her tactician has a judgement for: struck out
    CL_GS_CLOCK        = 4, // marked, on a timer or a chance: struck out
    CL_GS_MISFIT       = 6, // an action that cannot be aimed at the side its condition names: struck out
};

enum
{
    CL_GO_OWN  = 0, // her own row
    CL_GO_LENT = 1, // a row her party role lends her (RESEARCH §17): shown, never edited
    CL_GO_BOTH = 2, // the role's row standing in the place of one of hers that meant the same: shown under her number, its content pinned, moved as hers; hers comes back when the role goes
};

// One of her rows, as the editor shows it: an answer to GAMBITS and to every
// edit. The rows come in the running order, her party role's rows fitted in
// among hers; index numbers her own, and is 0 on a lent row, which no edit
// names
typedef struct cl_gambit_row
{
    cl_header h;
    uint32_t  cardian;   // charid
    uint8_t   index;     // 1-based among her own rows; 0 on a lent row
    uint8_t   on;        // as it runs: her checkbox, or on for a row her party role pins
    uint8_t   state;     // CL_GS_*: what the row means where it sits
    uint8_t   fits;       // 0: more than cl_gambit carries (a hand-made brain's), gambit left empty: shown, not rewritten
    uint8_t   origin;    // CL_GO_*
    uint8_t   lender;    // CL_ROLE_*: the party role a lent row comes from, or that hers folds; CL_ROLE_NONE on her own
    cl_gambit gambit;
    char      head[64];   // the row as the player reads it: when, "Ally: HP < 50%"
    char      action[64]; // and what, "Cure (best)"
} cl_gambit_row;

// Her rows: each a GAMBIT_ROW (CL_F_MORE), then this
typedef struct cl_gambits
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   master;  // answered: 1 while her master switch is on
    uint8_t   count;   // answered: rows sent
    uint8_t   spare[2];
} cl_gambits;

typedef struct cl_gambit_toggle
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   index;
    uint8_t   on;
    uint8_t   spare[2];
} cl_gambit_toggle;

typedef struct cl_gambit_move
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   from;    // 1-based, as shown
    uint8_t   to;
    uint8_t   spare[2];
} cl_gambit_move;

typedef struct cl_gambit_delete
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   index;
    uint8_t   spare[3];
} cl_gambit_delete;

// A new row at a position, switched on
typedef struct cl_gambit_insert
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   index;
    uint8_t   spare[3];
    cl_gambit gambit;
} cl_gambit_insert;

// A row rewritten in place, keeping its switch
typedef struct cl_gambit_replace
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   index;
    uint8_t   spare[3];
    cl_gambit gambit;
} cl_gambit_replace;

typedef struct cl_gambit_master
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   on;
    uint8_t   spare[3];
} cl_gambit_master;

// ---- the pickers' catalogue: every clause a row can say, for her ----------

enum
{
    CL_VC_NOTHING = 0, // the clause takes nothing more
    CL_VC_NUMBER  = 1, // a number in its range, the label's '*'
    CL_VC_STATUS  = 2, // a status, picked in the row's next cell
};

enum
{
    CL_SIDE_SELF = 0,
    CL_SIDE_ALLY = 1,
    CL_SIDE_FOE  = 2,
};

// A clause: one target and one condition, on its side's page
typedef struct cl_vocab_condition
{
    uint16_t target;
    uint16_t condition;
    uint8_t  takes;     // CL_VC_*
    uint8_t  side;      // CL_SIDE_*
    uint16_t spare;
    uint16_t min;       // CL_VC_NUMBER: its range, its step, and where a new row starts
    uint16_t max;
    uint16_t step;
    uint16_t initial;
    char     label[48]; // for people; with CL_VC_NUMBER a '*' marks where the number is shown
} cl_vocab_condition;

typedef struct cl_vocab_conditions
{
    cl_header          h;
    uint32_t           cardian; // charid
    uint8_t            count;
    uint8_t            spare[3];
    cl_vocab_condition conditions[48];
} cl_vocab_conditions;

// A status a status condition can name
typedef struct cl_vocab_status
{
    uint16_t id;        // xi::StatusEffect
    char     label[30];
} cl_vocab_status;

typedef struct cl_vocab_statuses
{
    cl_header       h;
    uint32_t        cardian; // charid
    uint8_t         count;
    uint8_t         spare[3];
    cl_vocab_status statuses[48];
} cl_vocab_statuses;

enum
{
    CL_AG_FIGHT         = 0,
    CL_AG_BEHAVIOURS    = 1,
    CL_AG_MAGIC         = 2,
    CL_AG_ABILITIES     = 3,
    CL_AG_WEAPON_SKILLS = 4,
    CL_AG_RANGED        = 5,
};

// An action a row can take, of her main and support job at every level
typedef struct cl_vocab_action
{
    cl_gambit_action action;
    uint16_t         targets;   // what it may be aimed at (the server's TARGET_* flags)
    uint16_t         mp;        // a spell's base MP cost
    uint8_t          group;     // CL_AG_*
    uint8_t          usable;    // 1: she can use it now; the pickers grey the rest
    uint16_t         spare;
    char             label[48];
} cl_vocab_action;

typedef struct cl_vocab_actions
{
    cl_header       h;
    uint32_t        cardian; // charid
    uint8_t         count;
    uint8_t         spare[3];
    cl_vocab_action actions[48];
} cl_vocab_actions;

// The catalogue: its clauses, statuses and actions as answers in parts
// (CL_F_MORE), then this, with her jobs and levels, which the actions follow
typedef struct cl_gambit_vocab
{
    cl_header h;
    uint32_t  cardian;   // charid
    uint8_t   mainJob;   // answered
    uint8_t   mainLevel; // answered
    uint8_t   subJob;    // answered
    uint8_t   subLevel;  // answered
} cl_gambit_vocab;

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
    CL_T_WAIT         = 0x040B,
    CL_T_RESCUE       = 0x040C,
    CL_T_HOMEPOINT    = 0x040D,
    CL_T_CANCEL       = 0x040E,
    CL_T_DO           = 0x040F,
    CL_T_QUEUES       = 0x0410,
    CL_T_SPAWN        = 0x0411,
    CL_T_DESPAWN      = 0x0412,
    CL_T_PARTY_ROLES    = 0x0413,
    CL_T_PARTY_ROLE     = 0x0414,
    CL_T_SET_PARTY_ROLE = 0x0415,
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

// Wait here, or follow him: from another zone, following is a trek to his.
// Answered by the outcome alone; her roster line says it next time it is read.
typedef struct cl_wait
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   on;      // 1 wait here, 0 follow
    uint8_t   spare[3];
} cl_wait;

// A stuck cardian to his side: only from within pawn.RESCUE_RANGE yalms, on
// pawn.RESCUE_COOLDOWN shared by all his cardians. Answered by the outcome,
// with the numbers behind a refusal.
typedef struct cl_rescue
{
    cl_header h;
    uint32_t  cardian;      // charid
    float     away;         // answered with CL_S_TOO_FAR: yalms between them
    float     range;        // answered with CL_S_TOO_FAR: the rescue's reach
    uint16_t  cooldownLeft; // answered with CL_S_COOLING_DOWN: seconds
    uint16_t  spare;
} cl_rescue;

// A KO'd cardian to his home point, where she waits
typedef struct cl_homepoint
{
    cl_header h;
    uint32_t  cardian; // charid
} cl_homepoint;

// A queued command taken back: the one a character has waiting -- the
// player's through a pause, a cardian's through a pause or behind what she is
// doing. The queue line that changes is told as it always is; this answers
// with the outcome alone.
typedef struct cl_cancel
{
    cl_header h;
    uint32_t  cardian; // charid; 0 = the player's own
} cl_cancel;

// The command window: one action now, on a target. An order she cannot start
// at once -- busy, on recast, out of reach, the game paused -- is held as her
// one queued order within cardian.ORDER_GRACE, the server's 2.5 s after a spell
// added on (QUEUE tells it); one that could not start in that time is refused
// CL_S_TOO_SOON. Answered by the outcome.
typedef struct cl_do
{
    cl_header h;
    uint32_t  cardian; // charid
    cl_action action;
    uint16_t  target;  // a target index in her zone; 0 = herself
    uint16_t  wait;    // answered with CL_S_TOO_SOON: seconds until it could start
} cl_do;

// The Debug screen's spawn and despawn of one of his (OWNED; creation stays
// !pawncreate): she stands beside him, or leaves the world
typedef struct cl_spawn
{
    cl_header h;
    uint32_t  cardian; // charid
} cl_spawn;

typedef struct cl_despawn
{
    cl_header h;
    uint32_t  cardian; // charid
} cl_despawn;

// Every queue line as it stands, for an addon that has just bound: each
// character with a command waiting, his own and his cardians', comes as a QUEUE
// answer (CL_F_MORE), then this
typedef struct cl_queues
{
    cl_header h;
} cl_queues;

// ---- the party's roles (RESEARCH §17) ----
// Who tanks, heals, deals damage and pulls, as the party screen shows them.
// The join rule says each member's role until the player chooses one for her.
enum
{
    CL_ROLE_NONE   = 0,
    CL_ROLE_TANK   = 1,
    CL_ROLE_HEALER = 2,
    CL_ROLE_DAMAGE = 3,
    CL_ROLE_PULLER = 4, // one member's; the others can be held by several
    CL_ROLE_AUTO   = 255, // asked only, never answered: the player's choice for her is taken back, and the join rule decides again
};

// One member of his party, her role, and what her column of the party screen
// shows of her: an answer to PARTY_ROLES and to SET_PARTY_ROLE
typedef struct cl_party_role
{
    cl_header h;
    uint32_t  member;    // charid: the player, a cardian, or another player of his party
    char      name[16];
    uint8_t   mainJob;
    uint8_t   mainLevel;
    uint8_t   subJob;
    uint8_t   subLevel;
    uint8_t   role;      // CL_ROLE_*, never CL_ROLE_AUTO
    uint8_t   byPlayer;  // 1 when the player chose it, 0 when the join rule gave it
    uint8_t   self;      // 1 on the asking player's own row
    uint8_t   spare;
    uint16_t  hp;
    uint16_t  maxHp;
    uint16_t  mp;
    uint16_t  maxMp;
    uint16_t  tp;
    uint16_t  attack;
    uint16_t  defence;
    uint16_t  spare2;
    int16_t   total[7];  // STR, DEX, VIT, AGI, INT, MND, CHR
    int16_t   bonus[7];  // the part of each that gear and effects give
    uint16_t  worn[16];  // the item in each equipment slot, main hand to back; 0 for an empty slot
} cl_party_role;

// His party's roles: each member comes as a PARTY_ROLE answer (CL_F_MORE), the
// player first and then in the order the party holds them, then this
typedef struct cl_party_roles
{
    cl_header h;
    uint8_t   count;    // answered: members sent
    uint8_t   spare[3];
} cl_party_roles;

// The player's choice of a member's role, or, with CL_ROLE_AUTO, his taking
// it back. The roles come back as they now stand (PARTY_ROLE, CL_F_MORE) ahead
// of the outcome, refused or not
typedef struct cl_set_party_role
{
    cl_header h;
    uint32_t  member;   // charid
    uint8_t   role;     // CL_ROLE_*
    uint8_t   spare[3];
} cl_set_party_role;

// ---- 0x05xx: the pause and the server's other notices ---------------------

enum
{
    CL_T_PAUSED         = 0x0501,
    CL_T_RESUMED        = 0x0502,
    CL_T_CALENDAR       = 0x0503,
    CL_T_MANEUVER_STATE = 0x0504,
    CL_T_WALK_TAKEN     = 0x0505,
    CL_T_PAUSE          = 0x0506,
    CL_T_QUEUE          = 0x0507,
    CL_T_NOTE           = 0x0508,
};

enum
{
    CL_NOTE_LET_GO    = 0, // she let the order go: reason says why
    CL_NOTE_REFUSED   = 1, // the game refused it on its next step: message
    CL_NOTE_REST_ENDS = 2, // her rest order ended: reason says why
};

// One-way, to the player whose order it was: what came of one of his cardians'
// orders after it was taken, which she has no client to show. The header's
// status says why. Let go -- CL_S_NO_TARGET its target gone, CL_S_UNREACHED she
// could not get in reach of it, CL_S_TOO_SOON busy or on recast past the
// order's grace (wait: the recast's seconds left, 0 busy), or what the game
// answered when she tried; refused by the game on its next step, with the
// battle message it answered; or her rest order ended. The server names it;
// the addon words it.
typedef struct cl_note
{
    cl_header h;
    uint32_t  cardian;  // charid
    uint8_t   kind;     // CL_NOTE_*
    uint8_t   spare[3];
    cl_action action;   // the order
    uint16_t  target;   // CL_S_UNREACHED: the target's index in her zone
    uint16_t  wait;     // CL_S_TOO_SOON: seconds of recast left; 0 busy
    uint16_t  message;  // CL_NOTE_REFUSED: the game's battle message (MsgBasic)
    uint16_t  spare2;
    char      about[32]; // for people: the battle message's name (REFUSED), the target's (UNREACHED)
} cl_note;

// One-way, to a player at every change of a queue line of his (and an answer
// to QUEUES): the one command a character has waiting -- one of his cardians'
// orders, held behind what she is doing or through a pause, or his own command
// through a pause. The server names it; the addon words it.
typedef struct cl_queue
{
    cl_header h;
    uint32_t  character; // charid: one of his cardians, or himself
    cl_action action;    // CL_AK_NONE: nothing waits now
    uint16_t  target;    // the target's index in the zone; 0 when it is gone
    uint16_t  spare;
} cl_queue;

// The pause button: takes the hold, or lets go of his own. Answered by the
// outcome; the hold itself is told to every addon (PAUSED, RESUMED).
typedef struct cl_pause
{
    cl_header h;
} cl_pause;

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

// ---- 0x06xx: the party finder ----------------------------------------------
//
// The world's adventurers, recruited (ROADMAP H, party_finder.h): the shout,
// a look at one who answered, the invite, and his open contracts. A goal is
// what he recruits for: experience, or a mission log, or a quest area.

enum
{
    CL_T_SHOUT           = 0x0601,
    CL_T_SHOUT_RESPONDER = 0x0602,
    CL_T_PEEK            = 0x0603,
    CL_T_INVITE          = 0x0604,
    CL_T_CONTRACTS       = 0x0605,
    CL_T_END_CONTRACT    = 0x0606,
    CL_T_GOALS           = 0x0607,
    CL_T_GOAL            = 0x0608,
};

enum
{
    CL_GOAL_EXP     = 0,
    CL_GOAL_MISSION = 1, // log: the mission log
    CL_GOAL_QUEST   = 2, // log: the quest area
};

enum
{
    CL_PRESENCE_HERE     = 0, // standing in his zone
    CL_PRESENCE_STANDING = 1, // standing elsewhere in his city
    CL_PRESENCE_BUSY     = 2, // in a party, standing or camping faded
    CL_PRESENCE_FADED    = 3, // online, no body: she stands for an invite
    CL_PRESENCE_AWAY     = 4,
};

enum
{
    CL_FIT_FREE   = 0, // between missions, free to take it
    CL_FIT_BEHIND = 1, // she has not reached the mission: a no of its own kind
    CL_FIT_ON     = 2, // on that very mission
    CL_FIT_DONE   = 3, // past it: she knows the way
};

// One who heard his shout, and her answer, fixed when the shout was made: an
// answer to SHOUT. She is heard revealMs after the shout and answers decideMs
// later; line is her words
typedef struct cl_shout_responder
{
    cl_header h;
    uint32_t  shout;        // the shout's id
    uint32_t  cardian;      // charid
    char      name[16];
    uint8_t   job;
    uint8_t   level;
    uint8_t   race;
    uint8_t   nation;
    uint8_t   rank;         // in her own nation
    uint8_t   presence;     // CL_PRESENCE_*
    uint8_t   willing;      // 1: she would come
    uint8_t   fit;          // CL_FIT_*: where her mission log stands, for a mission
    uint32_t  affinity;     // hers toward him
    uint32_t  revealMs;
    uint32_t  decideMs;
    uint16_t  zone;         // the zone id
    uint16_t  spare;
    char      zoneName[32]; // for people
    char      line[96];     // for people
} cl_shout_responder;

// The shout for a goal: up to eight adventurers in reach hear it and answer
// in their own time, each a SHOUT_RESPONDER (CL_F_MORE), then this. The same
// goal again without `again` is the shout he has, to replay; `again` is a new
// one, refused inside the cooldown with the one he has coming back. Refused
// CL_S_COOLING_DOWN with none, waitMs saying how long
typedef struct cl_shout
{
    cl_header h;
    uint8_t   goal;    // CL_GOAL_*; answered: the goal of the shout he has, another than asked when a re-shout was refused
    uint8_t   log;     // answered: the same
    uint8_t   again;   // 1: a new shout, not the one he has
    uint8_t   count;   // answered: responders sent
    uint32_t  id;      // answered: this shout; the same id is the same shout
    uint32_t  waitMs;  // answered: until he may shout again
} cl_shout;

// A look at one who answered his shout, or whom his contract holds: her jobs,
// nation and rank, her affinity, what she wears -- as she stands, or as the
// census dressed her while faded -- and her numbers as she stands, or as she
// last stood at this level (known 0: never seen)
typedef struct cl_peek
{
    cl_header h;
    uint32_t  cardian;   // charid
    uint8_t   job;       // answered
    uint8_t   level;
    uint8_t   subJob;
    uint8_t   subLevel;
    uint8_t   nation;
    uint8_t   rank;
    uint8_t   standing;  // 1: she has a body in the world
    uint8_t   known;     // 1: the numbers below are hers
    uint32_t  affinity;
    uint16_t  hp;
    uint16_t  maxHp;
    uint16_t  mp;
    uint16_t  maxMp;
    int16_t   total[7];  // STR, DEX, VIT, AGI, INT, MND, CHR
    int16_t   bonus[7];
    uint16_t  attack;
    uint16_t  defence;
    uint16_t  items[16]; // what she wears, by equipment slot; 0 for none
} cl_peek;

// The party invite he would send by hand, sent for him, for the goal she was
// asked about; she answers it herself. Answered by the outcome; declined,
// with her words
typedef struct cl_invite
{
    cl_header h;
    uint32_t  cardian;  // charid
    uint8_t   goal;     // CL_GOAL_*
    uint8_t   log;
    uint8_t   spare[2];
    char      line[96]; // answered with CL_S_DECLINES: hers, for people
} cl_invite;

enum
{
    CL_CONTRACT_PARTY    = 0, // in his party
    CL_CONTRACT_STANDING = 1, // standing, waiting to be invited
    CL_CONTRACT_FADED    = 2, // online, no body
    CL_CONTRACT_OUT      = 3, // could not stand (the map log says why)
};

// One of his open contracts: a world's adventurer held for him
typedef struct cl_contract
{
    uint32_t cardian;  // charid
    uint8_t  goal;     // CL_GOAL_*: what she was recruited for
    uint8_t  job;
    uint8_t  level;
    uint8_t  state;    // CL_CONTRACT_*
    uint16_t zone;     // the zone id
    uint16_t spare;
    char     name[16];
} cl_contract;

typedef struct cl_contracts
{
    cl_header   h;
    uint8_t     count;         // answered
    uint8_t     spare[3];
    cl_contract contracts[16]; // answered
} cl_contracts;

// Her Party Finder page's Release: the contract ends and she is the world's
// again where she stands, out of his party first when she is in it
typedef struct cl_end_contract
{
    cl_header h;
    uint32_t  cardian; // charid
} cl_end_contract;

// One goal he could recruit for: a mission log's current mission, or a quest
// under way. An answer to GOALS
typedef struct cl_goal
{
    cl_header h;
    uint8_t   kind;      // CL_GOAL_MISSION or CL_GOAL_QUEST
    uint8_t   log;       // the mission log, or the quest area
    uint16_t  id;        // the mission's or the quest's id in its log
    char      title[48]; // for people
} cl_goal;

// What he could recruit for (the finder's goals): each GOAL (CL_F_MORE), then
// this, with how many missions of each log he has completed
typedef struct cl_goals
{
    cl_header h;
    uint8_t   count;         // answered: goals sent
    uint8_t   spare[3];
    uint16_t  completed[16]; // answered: by mission log id
} cl_goals;

// ---- 0x07xx: the conquest exchange -----------------------------------------
//
// A cardian cannot talk to a gate guard, but her player can stand beside one
// (the roster's CL_MEMBER_BY_GUARD), and she buys from that guard's stock, at
// that guard's prices, out of her own conquest points. The sale is the
// guard's own (scripts/globals/conquest.lua); the Link asks
// modules/cardian/lua/conquest_exchange.lua.

enum
{
    CL_T_CP_SHOP = 0x0701,
    CL_T_CP_ITEM = 0x0702,
    CL_T_CP_BUY  = 0x0703,
};

// One thing the guard sells her: an answer to CP_SHOP
typedef struct cl_cp_item
{
    cl_header h;
    uint16_t  option; // the guard's own number for it
    uint16_t  item;   // the item id
    uint32_t  price;  // in her conquest points, at this guard
    uint8_t   level;  // the level it is worn at
    uint8_t   rank;   // the rank in her nation it needs; 0 for none
    uint8_t   place;  // the conquest place her nation must hold for it; 0 for none
    uint8_t   spare;
} cl_cp_item;

// The guard within the player's reach: what he sells her, each a CP_ITEM
// (CL_F_MORE), then this, where she stands with him
typedef struct cl_cp_shop
{
    cl_header h;
    uint32_t  cardian;     // charid
    uint32_t  cp;          // answered: her conquest points
    uint8_t   rank;        // answered: her rank in her nation
    uint8_t   nation;      // answered: hers (xi.nation)
    uint8_t   guardNation; // answered: the guard's; 4 for Jeuno's
    uint8_t   nationRank;  // answered: her nation's place in the conquest tally
    uint8_t   foreign;     // answered: 1, another nation's guard
    uint8_t   blocked;     // answered: 1, his nation outranks hers, so he sells her nothing
    uint8_t   count;       // answered: items sent
    uint8_t   spare;
    char      guard[24];   // answered: the guard's name as the game keeps it ('Aravoge_TK'), for people
} cl_cp_shop;

// Buy one thing for her: her inventory (INVENTORY, CL_F_MORE) when she bought
// it, then this. A refusal's have and need are the numbers its words need: her
// points and the price, her rank and the item's, or her nation's place and
// the item's
typedef struct cl_cp_buy
{
    cl_header h;
    uint32_t  cardian; // charid
    uint16_t  option;  // cl_cp_item's
    uint16_t  spare;
    uint32_t  cp;      // answered: her conquest points now
    uint32_t  have;    // answered, with a refusal that needs it
    uint32_t  need;
} cl_cp_buy;

// ---- 0x08xx: the Auction House ---------------------------------------------
//
// The Auction House screen (ROADMAP L) shops for a member of the player's
// party: the player himself, by his own charid, or a cardian of his to manage.
// He must stand by an auction counter, and she by the counter he stands at.
// The auction house stays blind, as retail's: a listing's price is never told,
// only how many are listed and what the last sales paid.

enum
{
    CL_T_AH_SHELF   = 0x0801,
    CL_T_AH_HISTORY = 0x0802,
    CL_T_AH_BID     = 0x0803,
};

// One item in one form the auction house lists it in: singly, or by the stack
typedef struct cl_ah_listing
{
    uint16_t item;
    uint8_t  level;     // the level it asks for: gear's, a scroll's to learn; else 0
    uint8_t  category;  // the auction house's own (xi.itemAHCategory)
    uint32_t stock;     // how many are listed now in this form; 0 when sold out
    uint32_t going;     // the going rate in this form; 0 when nothing has sold
    uint8_t  stack;     // 1: the stack form
    uint8_t  spare;
    uint16_t stackSize; // how many pieces the form buys
} cl_ah_listing;

enum
{
    CL_SHELF_SLOT       = 0, // what she can wear in `slot`
    CL_SHELF_CATEGORIES = 1, // everything ever listed in `categories`
    CL_SHELF_LEARNABLE  = 2, // the spell scrolls in `categories` she can learn now and has not
};

// A shelf for one member: every item the auction house has ever listed there,
// in stock or sold out, one row per form, in the auction house's order (by
// category as asked, the highest level first). Answered in parts, each with
// the request's own fields (CL_F_MORE), the last with the outcome.
typedef struct cl_ah_shelf
{
    cl_header     h;
    uint32_t      member;         // charid
    uint8_t       kind;           // CL_SHELF_*
    uint8_t       slot;           // CL_SHELF_SLOT: the equipment slot
    uint8_t       count;          // categories used
    uint8_t       rows;           // answered: listings used in this part
    uint8_t       categories[16]; // the auction house's own, in the order shown
    cl_ah_listing listings[64];   // answered
} cl_ah_shelf;

// One sale off an item's history
typedef struct cl_ah_sale
{
    uint32_t date;       // Unix time
    uint32_t price;      // what the buyer paid
    char     seller[16];
    char     buyer[16];
} cl_ah_sale;

// What the game's own auction house shows of an item in one form: its stock,
// the going rate, and its last ten sales, newest first. Asked by the buy panel.
typedef struct cl_ah_history
{
    cl_header  h;
    uint16_t   item;
    uint8_t    stack;     // 1: the stack form
    uint8_t    count;     // answered: sales used
    uint32_t   stock;     // answered
    uint32_t   going;     // answered
    cl_ah_sale sales[10]; // answered
} cl_ah_history;

// A bid for one piece or one stack, as the game's own purchase: the cheapest
// listing at or under the bid is hers, and she pays the bid. A cardian's purse
// has his behind it (fromPurse). Won, the piece goes on from her inventory to
// `bag` and, with `equip`, she wears it. Answered with the outcome; won, with
// where it landed and whether she wears it.
typedef struct cl_ah_bid
{
    cl_header h;
    uint32_t  member;    // charid
    uint32_t  price;
    uint32_t  fromPurse; // answered: the part of the price his purse gave
    uint16_t  item;
    uint8_t   stack;     // 1: the stack form
    uint8_t   bag;       // the inventory, or a bag she carries into the field; answered: where it landed
    uint8_t   slot;      // with equip: the equipment slot
    uint8_t   equip;     // 1: wear it
    uint8_t   equipped;  // answered
    uint8_t   spare;
    uint16_t  notWorn;   // answered with equip and equipped 0: why (CL_S_*)
    uint16_t  spare2;
} cl_ah_bid;

#pragma pack(pop)
