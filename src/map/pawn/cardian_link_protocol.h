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
//     the party finder, 0x07 the conquest exchange, 0x08 the Auction House,
//     0x09 the Mog House, 0x0A the linkshell, 0x0B the linkshell bank.
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
// compatible, the user, 2026-09-14). 93: SHOUT_RESPONDER's sjob and slvl,
// her support job on the shout's list; 92: MOBS_NEAR's kinds and MOB_NEAR's
// ally, the members of his party near her too, for a friendly far pick; 91: MOBS_NEAR and MOB_NEAR, the monsters
// near a party member as the server has them (a far pick's list), and
// CL_LENDER_PARTY, the party's Avoid aggro lent on a gambit page; 90: PREFS' three calls are the game's
// <call0> to <call20> by their own numbers, and CL_CALL_NONE (0xFF) is none,
// apart from Call 0; 89: SUPPLIES' WANTED and FOREIGN
// retired, and its nation and missing with them -- a cardian runs her own
// errands in town; 88: PREFS' chainCall and readyCall, the
// call sounds of the skillchain plan's calls (Settings); 87: the Pull page and the puller's badge --
// PULL_KINDS, PULL_KIND, SET_PULL_LIST, PULL_STATE and CL_S_CANNOT_PULL;
// 86: PREFS' pullCall, the call sound of
// the puller's pull line (Settings, Pull call); 85: the Shortcuts menu's Skillchain
// now -- SKILLCHAIN_NOW, CL_S_NO_PLAN and CL_S_NO_FIGHT; 84: the Shortcuts
// menu -- ORDERS' avoidAggro, SET_HUNT's CL_HUNT_AVOID_AGGRO, SKILLCHAINS, SKILLCHAIN and
// SET_SKILLCHAIN, and CL_S_NO_SKILLCHAIN; 83: DBOX's loc, a stack sent from any
// bag of hers; 82: DBOX, a cardian's delivery box, and
// CL_S_DBOX_BUSY to CL_S_ALREADY_SENT; 81: AH_REPRICE, a listing of his given a
// new price; 80: selling on the auction house --
// AH_QUOTES, AH_LISTINGS, AH_SELL and AH_CANCEL, AH_HISTORY's start, and
// CL_S_NOT_AUCTIONABLE to CL_S_LISTING_GONE; 79: BANK_JUNK, the player's haul into
// the bank at once; 78: the bank's moves made safe --
// BANK_WITHDRAW's version, CLUB's and BANK's shell name widened to 24,
// CL_S_NOT_WITH_YOU; 77: the shell's bank -- BANK and
// BANK_HOLDER reshaped (bags, gil, ranks), BANK_GIL, BANK_RIGHTS, CLUB's
// shell name, colour and bank; 76: the linkshell bank -- BANK,
// BANK_HOLDER, BANK_DEPOSIT and BANK_WITHDRAW, CLUB's bank, CL_S_BANK_CLOSED
// and CL_S_BANK_FULL; 75: a venture's time made up after the map was down --
// VENTURE_NEWS's minutes, CL_NEWS_CAUGHT_UP; 74: CL_LENDER_TREK, a gambit row her trek
// lends; 73: a broken rod that takes the lure --
// VENTURE_NEWS's lure, CL_NEWS_ROD_LURE_LOST; 72: a venture's supplies by item --
// CL_VENTURE_USED and CL_VENTURE_BOUGHT; 71: a lure lost to a snapped line --
// SEND_ERRAND's and PROFESSION's endOnSnap, VENTURE_BAIT's lure,
// CL_NEWS_LURE_LOST; 70: CL_S_KEEPER_DOWN gone (0x01EF is not
// used again), the venture keeper retired; 69: VENTURE_SPOT's name and
// VENTURE_REPORT's areaName 48 long, a water's whole name; 68: PROFESSION_TOOL's and VENTURE_BAIT's
// price the whole purchase's (a rod, or a stack of bait); 67: a venture's supplies bought where
// they cost least, and no estimates -- PROFESSION_TOOL's, VENTURE_SPOT's and
// VENTURE_BAIT's gilHour gone; 66: CLUB_MEMBER's ventureDone, a
// venture over and its report unread; CL_VENTURE_KEPT; CL_NEWS_NO_KIT and
// CL_NEWS_CANNOT_FISH; 65: a report per venture --
// VENTURE_REPORT's length, earned, pending, supplies and spent in place of
// waiting, gilEarned, unsoldValue and hisGil, VENTURE_ITEM's earlier, and
// CL_VENTURE_CAUGHT; 64: PROFESSIONS' debugLength, the dev
// profile's one-minute venture; every bait and lure of the game in
// VENTURE_BAITS; 63: PROFESSIONS' freeSlots, her open
// inventory spaces; 62: items let go on a fishing venture --
// SEND_ERRAND's letItemsGo, PROFESSION's; 61: a venture's catch sold --
// CL_VENTURE_RETURNED; 60: the map fishes a venture itself --
// CL_NEWS_BAGS_FULL and CL_NEWS_NO_GIL, and CL_ERRAND_RETURNING gone (3 is
// not used again); 59: a venture's spots by zone, nearest
// first -- VENTURE_SPOT's hops, its length 1 or 2 hours; 58: the profession
// set filled by hand --
// PROFESSION's rank, PROFESSION_TOOLS' slot and CL_TOOLS_SLOT, CL_TOOL_GEAR,
// PROFESSION_SLOT; 57: the fishing set and its kit chosen
// on the venture -- PROFESSION's set, PROFESSION_TOOLS' which, zone and area,
// PROFESSION_TOOL's kind and gilHour, VENTURE_BAITS' rod, SEND_ERRAND's rod,
// VENTURE_PAID's quantity and both purses, VENTURE_NEWS, START_PROFESSION
// without a tool; 56: professions and the fishing
// venture -- PROFESSIONS, PROFESSION, PROFESSION_TOOLS, PROFESSION_TOOL,
// START_PROFESSION, VENTURE_SPOTS, VENTURE_SPOT, VENTURE_BAITS, VENTURE_BAIT,
// VENTURE_PAID, VENTURE_REPORT and VENTURE_ITEM, SEND_ERRAND's zone, area,
// bait and hours, CLUB_MEMBER's can2, CL_ERRAND_RETURNING and the
// profession outcomes; 55: the linkshell page's ventures,
// recruits and the game's own trade -- CLUB_MEMBER's levels (her career),
// secondsTotal, missionsDone, missions, affinity and missionsTogether, and
// CL_CLUB_RECRUIT in CL_CLUB_GUEST's place; CLUB_RECRUIT and NOT_QUALIFIED;
// PEEK's missions; PEARL only breaks (a pearl is given by the game's own
// trade window); 54: the linkshell on the game's items --
// CLUB's shell, pearls and vendor, CLUB_MEMBER's nation, rank and rankCap,
// ERRAND_GOAL's rank and missions, SEND_ERRAND's rank, CL_ERRAND_RANK,
// CL_CAN_SEND_RANK and NO_PEARL_TO_GIVE; 53: the linkshell, CLUB, CLUB_MEMBER,
// CLUB_INVITE, PEARL, ERRANDS, ERRAND_GOAL, SEND_ERRAND and CALL_BACK, and
// their outcomes; 52: a worn piece's charges and readyIn, in
// GEAR's cl_worn and PARTY_ROLE's wornCharges and wornReadyIn, so every gear
// tooltip shows an enchanted piece's line; 51: SUPPLIES, a cardian's conquest scrolls
// bought, wanted or refused; 50: a warp together's member's ring and
// cudgel, each piece's charges and recast left (cl_offer_piece); 49: the Warp
// Ring and the Warp Cudgel among the ways home (CL_WAY_RING, CL_WAY_CUDGEL); 48:
// a warp together's member's barred, the ways she knows but cannot use now; 47:
// the warp together's picks, OFFER's own
// and each member's ways, and OFFER_ANSWER's ways, his pick for each; 46: the warp
// together, OFFER's members and each one's way home (CL_WAY_*), and OFFER_ANSWER's
// choice in place of its yes; 45: STAKE's front and back, the camp's front line
// and backline spacing; 44: a PARTY_ROLE's food, what the member eats
// in her party role (RESEARCH §19); 43: an item's charges and readyIn, an enchanted
// piece's charges left and the seconds until it can be used; the finder's LEFT_TOWN; 42: QUEUE's lane, the enchanted item she is
// putting on, waiting out and using (#297), ENCHANT_BUSY and NO_CHARGES; 41: PREFS, the player's preferences the
// server applies to him (Shared maps); 40: QUEUE's running, the order she is
// carrying out now; 39: Use, Give & use's use and Rescue join her line
// (CL_AK_RESCUE on it), and WALK's place, the camp's ring; 38: a cardian's queue four deep, QUEUE's
// next[] behind its first command, and QUEUE_FULL; 37: a cardian's rest on her queue line,
// her own (CL_AK_OWN_REST) or his order's (CL_AK_REST), and CANCEL calls it
// off; the camp placed at a spot, STAKE's CL_STAKE_AT; 36: the server's yes-or-no questions,
// OFFER and OFFER_ANSWER (the party's warp); 35: the Mog House, JOB_CHANGE, the
// roster's CL_MEMBER_BY_MOG_HOUSE, and JOBS' unlocked; 34: the orders' Dia or Bio (ORDERS'
// diaBio, CL_HUNT_DIA_BIO); 33: the gambit messages name the player
// himself too (his own set), and CL_GS_CLIENT; 32: ROLE_LOCKED, and a GAMBIT_ROW's on
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
enum { CL_PROTOCOL = 93 };

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
    CL_S_CAMP_TOO_FAR      = 0x0127, // a camp placed beyond pawn.STAKE_PLACE_REACH of him
    CL_S_CAMP_OFF_MESH     = 0x0128, // no ground to stand a camp on at the spot placed

    // One cardian's orders
    CL_S_NOT_KNOCKED_OUT   = 0x0130, // a home point is for a KO'd cardian
    CL_S_NOT_WHILE_PAUSED  = 0x0131, // a held simulation moves nobody
    CL_S_TOO_FAR           = 0x0132, // beyond the rescue's reach: the answer's away and range
    CL_S_COOLING_DOWN      = 0x0133, // a cooldown runs: the answer says how long (RESCUE's cooldownLeft, SHOUT's waitMs)
    CL_S_NOTHING_QUEUED    = 0x0134, // no command waits to be taken back
    CL_S_UNREACHED         = 0x0135, // she could not get in reach of the order's target, or holds her position out of a weapon skill's reach
    CL_S_CANNOT_RECOVER    = 0x0136, // resting, she cannot recover right now (a poison, an avatar out)
    CL_S_QUEUE_FULL        = 0x0137, // four commands wait already: take one back first

    // The pause button
    CL_S_PAUSE_OFF         = 0x0140, // the pause is switched off on this server
    CL_S_LOGGING_OUT       = 0x0141, // nobody pauses on his way out of the game
    CL_S_SYNTHESIZING      = 0x0142, // nor in the middle of a synthesis
    CL_S_FISHING           = 0x0143, // nor with a line in the water
    CL_S_PAUSED_BY_OTHER   = 0x0144, // another player holds it, and only its holder resumes

    // The command window's orders (DO), and using an item
    CL_S_TOO_SOON          = 0x0150, // no longer sent: an order waits however long (kept, never reused)
    CL_S_NOT_CARRIED       = 0x0151, // she carries none of that item
    CL_S_NOT_FIGHTING      = 0x0152, // a disengage with no fight to leave
    CL_S_NOT_MANAGED       = 0x0153, // her items are hers to use only when she is yours
    CL_S_CANNOT_NOW        = 0x0154, // the game would not start it now
    CL_S_ON_RECAST         = 0x0155, // its recast runs (an order waits it out in her line instead)
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
    CL_S_ENCHANT_BUSY      = 0x0175, // an enchanted item of hers is under way already: one at a time
    CL_S_NO_CHARGES        = 0x0176, // the enchanted item has no charges left

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
    CL_S_LEFT_TOWN         = 0x01A1, // she has left the cities and towns his shout reached

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

    // The Mog House (KO'd: CL_S_KNOCKED_OUT; an event, hers or his: CL_S_IN_EVENT)
    CL_S_NO_MOG_HOUSE      = 0x01C0, // the player is neither in his Mog House nor by a Nomad Moogle
    CL_S_JOB_LOCKED        = 0x01C1, // she has not unlocked that job
    CL_S_NO_SUPPORT_JOBS   = 0x01C2, // she has not unlocked support jobs
    CL_S_SAME_JOB          = 0x01C3, // the support job asked for is her main job
    CL_S_IN_A_FIGHT        = 0x01C4, // not while she fights
    CL_S_FAR_FROM_MOOGLE   = 0x01C5, // she stands beyond 20 yalms of the Nomad Moogle he stands by
    CL_S_DBOX_BUSY         = 0x01C6, // her delivery box cannot open while an item use or a synthesis of hers holds its container
    CL_S_NOT_DELIVERABLE   = 0x01C7, // the item may not be sent there: flagged so, or to another account only
    CL_S_BOX_BACKLOGGED    = 0x01C8, // the box it would go to has too much waiting for it already
    CL_S_NO_RECIPIENT      = 0x01C9, // no character by that charid
    CL_S_NO_SENDER         = 0x01CA, // no sender to return it to: the auction house sent it
    CL_S_ALREADY_SENT      = 0x01CB, // it is on its way: recall it first

    // The server's questions (OFFER)
    CL_S_OFFER_GONE        = 0x01D0, // the question is no longer open: answered, lapsed or withdrawn

    // The linkshell (an invite's own refusals are the party finder's: CL_S_PARTY_FULL, CL_S_NOT_LEADER...)
    CL_S_NOT_IN_CLUB       = 0x01E0, // not in his linkshell: an alt, a cardian he owns or one wearing his pearl (a pearl: also one of the world's in his party)
    CL_S_NO_LINKSHELL      = 0x01E1, // he holds no Linkshell or Pearlsack
    CL_S_HAS_PEARL         = 0x01E2, // she wears a pearl of his shell already
    CL_S_NO_PEARL          = 0x01E3, // she wears no pearl of his shell
    CL_S_PEARLED_ELSEWHERE = 0x01E4, // she wears a pearl of another shell
    CL_S_ON_ERRAND         = 0x01E5, // she is away on an errand
    CL_S_NO_ERRAND         = 0x01E6, // she is on no errand
    CL_S_NOT_OFFERED       = 0x01E7, // not an errand she can be sent on: a kind not built, or not for her, a quest he has not done, a rank not between hers and the cap
    CL_S_DONE_ALREADY      = 0x01E8, // she has done that already
    CL_S_NOT_STANDING      = 0x01E9, // she has no body in the world now
    CL_S_NO_PEARL_TO_GIVE  = 0x01EA, // he holds no Linkpearl of his shell to trade: he makes one from his linkshell first
    CL_S_NOT_QUALIFIED     = 0x01EB, // not a recruit for his linkshell: her affinity with him or their missions together short of the pearl's lock
    CL_S_NO_PROFESSION     = 0x01EC, // she has not taken up that profession
    CL_S_ONE_PROFESSION    = 0x01ED, // she has a profession already: one each, for now
    CL_S_NOT_SAFE          = 0x01EE, // the spot is not safe at her level, or is no spot of hers
    CL_S_NOT_A_TOOL        = 0x01F0, // not a tool that profession takes

    // The linkshell bank (her inventory full: CL_S_NO_SPACE; a Rare item she holds already: CL_S_RARE_OWNED)
    CL_S_BANK_CLOSED       = 0x0200, // the player stands by no auction counter, by no Nomad Moogle, and not in his Mog House
    CL_S_BANK_FULL         = 0x0201, // the bank has no room for the stack: its slots are all in use
    CL_S_NO_SHELL_WORN     = 0x0202, // he wears no linkshell item: no shell, no bank
    CL_S_BANK_NOT_ALLOWED  = 0x0203, // his rank in the shell may not do that (the holder's settings)
    CL_S_MOG_HOUSE_ONLY    = 0x0204, // that bag opens only in his Mog House
    CL_S_HOLDER_ONLY       = 0x0205, // only the shell's holder may change the bank's settings
    CL_S_NOT_WITH_YOU      = 0x0206, // that character is not where the place he stands at reaches (his city from his Mog House, near a moogle or a counter)

    // Selling on the auction house (his inventory full: CL_S_NO_SPACE; a Rare item he holds already: CL_S_RARE_OWNED;
    // too little gil for the fee: CL_S_NOT_ENOUGH_GIL; worn: CL_S_ITEM_EQUIPPED; a stack of an item that comes in none: CL_S_NOT_IN_STACKS)
    CL_S_NOT_AUCTIONABLE   = 0x0210, // the auction house takes no such item: flagged so (as Ex and most Rare items are), or with no place on its shelves
    CL_S_LISTING_LIMIT     = 0x0211, // he has as many listings on sale as he may
    CL_S_PARTLY_USED       = 0x0212, // an enchanted item with charges spent: the auction house takes it only full
    CL_S_NOT_A_FULL_STACK  = 0x0213, // a stack sells only full
    CL_S_LISTING_GONE      = 0x0214, // no listing of his on sale by that number: it sold, or came back to him

    // The party's skillchains (a member out of his party: CL_S_NOT_IN_PARTY)
    CL_S_NO_SKILLCHAIN     = 0x0220, // those two weapon skills make no skillchain, or a member cannot use hers now
    CL_S_NO_PLAN           = 0x0221, // Skillchain now with no skillchain picked
    CL_S_NO_FIGHT          = 0x0222, // Skillchain now with no mob being fought: by him, nor by his plan's opener or closer

    // The party's pulling
    CL_S_CANNOT_PULL       = 0x0230, // that member's job does not pull: a Puppetmaster, for now
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
    CL_AK_MOVE_WAIT   = 9,  // the same, then hold position at its end
    CL_AK_REST        = 10, // id: rest until this percent of HP and MP; mode 1 on a queue line: under way, not waiting
    CL_AK_CLIENT      = 11, // a player's own command from his client: id the action menu's (packet 0x01A's action id)
    CL_AK_HEAL        = 12, // a player's own /heal
    CL_AK_OWN_REST    = 13, // a cardian kneeling on her own -- her MP's pacing, her Rest row, beside the resting player -- no order of his; on her queue line only, mode 1
    CL_AK_RESCUE      = 14, // his Rescue waiting in her line (a pause, the cooldown, the orders ahead of it); on her queue line only
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
    CL_T_PREFS     = 0x0009,
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

// The player's preferences the server applies to him, from his addon after
// each bind and whenever one changes; without an addon each is the server's
// default. Answered by the outcome. A call is the game's own number, 0 to
// CL_CALL_LAST for <call0> to <call20>, or CL_CALL_NONE for none (the
// default); any other number is refused as malformed.
enum
{
    CL_CALL_LAST = 20,
    CL_CALL_NONE = 0xFF,
};

typedef struct cl_prefs
{
    cl_header h;
    uint8_t   sharedMaps; // 1 (the default): the maps any character of his account holds are shown to him too; 0: only his own
    uint8_t   pullCall;   // the call sound his puller's pull line ends with: <call0> to <call20>, or CL_CALL_NONE
    uint8_t   chainCall;  // the call sound the skillchain plan's opener ends "Starting {Skillchain}!" with: <call0> to <call20>, or CL_CALL_NONE
    uint8_t   readyCall;  // and its closer her "{Ready!} N TP": <call0> to <call20>, or CL_CALL_NONE
} cl_prefs;

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
    uint8_t  flags;   // CL_ITEM_*
    uint16_t id;
    uint32_t qty;
    uint8_t  charges; // an enchanted piece of gear: its charges left (its most is the client's to know); 0 for anything else
    uint8_t  spare;
    uint16_t readyIn; // an enchanted piece: seconds until it can be used -- its recast, and once worn the wait wearing it starts -- held at 65535
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
    CL_MEMBER_WAITING      = 0x01, // holding her ground, ordered or left behind by magic
    CL_MEMBER_OWNED        = 0x02, // his to manage; else a wild cardian in his party, orders only
    CL_MEMBER_BY_COUNTER   = 0x04, // she stands by the auction counter he stands at
    CL_MEMBER_BY_GUARD     = 0x08, // a gate guard stands within his reach: the conquest exchange sells to her
    CL_MEMBER_BY_MOG_HOUSE = 0x10, // he is in his Mog House, she in that city, or by a Nomad Moogle, she within 20 yalms of it: her jobs can be changed
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
    uint8_t   spare;
    uint16_t  homeIn;       // knocked out: seconds left of the game's hour before a fallen character is sent home; 0 otherwise
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
// (the inventory, or a wardrobe); item 0 for an empty equipment slot. An
// enchanted piece's charges and readyIn as cl_item's
typedef struct cl_worn
{
    uint16_t item;
    uint8_t  bag;
    uint8_t  slot;
    uint8_t  charges;
    uint8_t  spare;
    uint16_t readyIn;
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

// Her level in every job, by job id (0 for a job she has none in), and the
// jobs she has unlocked: a bit per job id, bit 0 her support jobs
typedef struct cl_jobs
{
    cl_header h;
    uint32_t  cardian;    // charid
    uint8_t   levels[24]; // answered
    uint32_t  unlocked;   // answered
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
// catalogue, for a cardian he commands -- or for himself, when `cardian` is
// his own charid: his own set, run by his hands while he plays (protocol 33;
// no behaviour in his catalogue). A row crosses as
// the gambit engine's own fields; its label is for people. Every edit is
// answered by her rows as they now stand -- each a GAMBIT_ROW, then GAMBITS
// (CL_F_MORE) -- and then its outcome, refused or not, so the editor never
// keeps a guess.

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
    CL_GS_CLIENT       = 7, // a behaviour row in the player's own list: only a cardian runs one, struck out
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
    uint8_t   on;        // as it runs: her checkbox, or a lent row's own: on for her party role's and her trek's, off for the party's Avoid aggro while his switch is
    uint8_t   state;     // CL_GS_*: what the row means where it sits
    uint8_t   fits;       // 0: more than cl_gambit carries (a hand-made brain's), gambit left empty: shown, not rewritten
    uint8_t   origin;    // CL_GO_*
    uint8_t   lender;    // CL_ROLE_*: the party role a lent row comes from, or that hers folds; CL_LENDER_TREK her trek's; CL_LENDER_PARTY the party's Avoid aggro switch's; CL_ROLE_NONE on her own
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
    CL_T_SKILLCHAINS    = 0x0416,
    CL_T_SKILLCHAIN     = 0x0417,
    CL_T_SET_SKILLCHAIN = 0x0418,
    CL_T_SKILLCHAIN_NOW = 0x0419,
    CL_T_PULL_KINDS     = 0x041A,
    CL_T_PULL_KIND      = 0x041B,
    CL_T_SET_PULL_LIST  = 0x041C,
    CL_T_PULL_STATE     = 0x041D,
    CL_T_MOBS_NEAR      = 0x041E,
    CL_T_MOB_NEAR       = 0x041F,
};

// One-way, a stream like pos: direct control's walk order (pawn.h), walk her
// to a point in the server's axes (x, height, z), sent every frame the
// player's ring moves; off takes the order back. The point is slid along her
// zone's mesh from the last one toward the one asked, so it never leaves floor
// she can walk. The server says nothing unless the mesh moved the point or
// the order was refused (WALK_TAKEN). With place, the ring is the camp being
// placed (STAKE at a spot): cardian 0, nobody walks, and the same mesh rule
// moves the point, which also stops within STAKE_PLACE_REACH of the player;
// off forgets that ring.
typedef struct cl_walk
{
    cl_header h;
    uint32_t  cardian; // charid; 0 with place
    float     x;
    float     y;
    float     z;
    uint8_t   off;     // 1: take the walk order back
    uint8_t   place;   // 1: the camp's ring, not a walk order
    uint8_t   spare[2];
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
    CL_MV_MOVE_WAIT = 3, // the same, then hold position at its end
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
    uint8_t   diaBio;     // answered: which of the two, which block each other, her tactician prefers: 0 Dia, 1 Bio
    uint16_t  stakeZone;  // answered: the camp's zone id
    uint8_t   avoidAggro; // answered: the Shortcuts menu's switch, 1 on (every member of his party avoids aggro, whatever her row says), 0 off (none does)
    uint8_t   spare[3];
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
    CL_HUNT_DIA_BIO    = 5, // value: 0 Dia, 1 Bio -- not a hunt rule but an order kept with them
    CL_HUNT_AVOID_AGGRO = 6, // value: 0 off, 1 on -- the Shortcuts menu's switch: Avoid aggro for every member of his party, an order kept with them
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
    CL_STAKE_AT     = 3, // set it, or move it, at x, y, z facing rotation: the camp placed as a maneuver
};

typedef struct cl_stake
{
    cl_header h;
    uint8_t   mode;     // CL_STAKE_*
    uint8_t   rotation; // CL_STAKE_AT: the camp's facing, the game's 0-255
    uint8_t   front;    // SET, TOGGLE, AT: the front line, yalms ahead of the flag a fight may settle before the tank tows it back (0: the server's)
    uint8_t   back;     // SET, TOGGLE, AT: the backline, yalms behind the flag the mages stand, never inside the mob's reach (0: the reach alone)
    float     x;        // CL_STAKE_AT: the spot, in the server's axes (y the height)
    float     y;
    float     z;
} cl_stake;

// Every cardian of his in his zone fights his target. Answered by the outcome alone.
typedef struct cl_engage
{
    cl_header h;
    uint16_t  target; // a target index in his zone
    uint16_t  spare;
} cl_engage;

// Hold position, or follow him: from another zone, following is a trek to his.
// Answered by the outcome alone; her roster line says it next time it is read.
typedef struct cl_wait
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   on;      // 1 hold position, 0 follow
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
// doing -- or, with nothing waiting, a cardian's spell of his she is still
// casting (QUEUE's running), broken off. The queue line that changes is told
// as it always is; this answers with the outcome alone.
typedef struct cl_cancel
{
    cl_header h;
    uint32_t  cardian; // charid; 0 = the player's own
} cl_cancel;

// The command window: one action now, on a target. An order she cannot start
// at once -- busy, on recast, out of reach, the game paused -- joins her line
// of queued orders, four deep, and waits there however long (QUEUE tells it);
// a fifth is refused CL_S_QUEUE_FULL. Answered by the outcome.
typedef struct cl_do
{
    cl_header h;
    uint32_t  cardian; // charid
    cl_action action;
    uint16_t  target;  // a target index in her zone; 0 = herself
    uint16_t  wait;    // unused: 0 (nothing is refused for its timing)
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

// A lent gambit row's lender that is no party role (GAMBIT_ROW's lender)
enum
{
    CL_LENDER_TREK  = 200, // her trek between zones: Avoid aggro and Avoid links, while it lasts
    CL_LENDER_PARTY = 201, // his Shortcuts menu's Avoid aggro, lent on or off to every member of his party
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
    uint16_t  food;      // the item she eats in her party role, 0 for none; on the player's own row, the food he has on now
    int16_t   total[7];  // STR, DEX, VIT, AGI, INT, MND, CHR
    int16_t   bonus[7];  // the part of each that gear and effects give
    uint16_t  worn[16];  // the item in each equipment slot, main hand to back; 0 for an empty slot
    uint16_t  wornReadyIn[16]; // an enchanted piece's readyIn, by equipment slot, as cl_item's
    uint8_t   wornCharges[16]; // and its charges left
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

// ---- the party's skillchains (ROADMAP, Road to subjob item 6, slice 3h) ----
// One skillchain the party can make: one member's weapon skill opens it and
// another's closes it, as the server would chain them. The members are the
// player and each cardian of his party in his zone whose weapon skill is her
// tactician's. An answer to SKILLCHAINS and to SET_SKILLCHAIN
typedef struct cl_skillchain
{
    cl_header h;
    uint32_t  opener;         // charid
    uint32_t  closer;         // charid
    uint32_t  burster;        // charid: the party's first member who can burst it -- a cardian whose nukes are her tactician's, a player who has learned one; 0 for none
    uint16_t  openerSkill;    // the weapon skill (the client's ability table by the same number)
    uint16_t  closerSkill;
    uint16_t  elements;       // the elements that burst it, a bit each: 1 << element, 1 fire to 8 dark
    uint8_t   chain;          // the skillchain, the server's numbers: 1 Transfixion .. 8 Impaction, 9 Gravitation .. 12 Fragmentation, 13 Light, 14 Darkness, 15 Light II, 16 Darkness II
    uint8_t   tier;           // 1 to 4
    uint8_t   picked;         // 1 for the plan he has picked
    uint8_t   spare[3];
    char      openerName[16];
    char      closerName[16];
    char      bursterName[16];
} cl_skillchain;

// The skillchains his party can make: each comes as a SKILLCHAIN answer
// (CL_F_MORE), the ones it can burst first and the higher tier first, then this
typedef struct cl_skillchains
{
    cl_header h;
    uint8_t   count;   // answered: options sent
    uint8_t   chain;   // answered: the chain of his plan, 0 for none; a plan none of the options is any more (a member's weapon changed) still answers it
    uint8_t   spare[2];
} cl_skillchains;

// His pick: the plan his party's tacticians' weapon skills follow, the chain
// the opener's weapon skill and the closer's make; opener 0 takes the plan
// back. The options come back as they now stand (SKILLCHAIN, CL_F_MORE)
// ahead of the outcome, refused or not. Refused: CL_S_NOT_IN_PARTY for a
// member not among those the options read, CL_S_NO_SKILLCHAIN for weapon
// skills that are not theirs now or make no chain
typedef struct cl_set_skillchain
{
    cl_header h;
    uint32_t  opener;      // charid, 0 for no plan
    uint32_t  closer;      // charid
    uint16_t  openerSkill;
    uint16_t  closerSkill;
    uint8_t   chain;       // answered: the chain of his plan as it now stands, 0 for none
    uint8_t   spare[3];
} cl_set_skillchain;

// His Skillchain now: on the mob being fought -- his own, else the one his
// plan's opener or closer fights -- the plan's members open and close as
// soon as they can, whatever the tactician's estimate of its worth says, for
// that fight. Refused: CL_S_NO_PLAN with no skillchain picked, CL_S_NO_FIGHT
// with no mob being fought
typedef struct cl_skillchain_now
{
    cl_header h;
    uint16_t  targid;      // answered: the mob's target index in his zone, 0 when refused
    uint8_t   spare[2];
} cl_skillchain_now;

// ---- the party's pulling (ROADMAP, Pulling) ----
// One kind of mob round the camp, a row of the Pull page: every mob of the
// name, or of a beastman race ("Goblins"), within CL_PULL_REACH of the camp
// (of the player, with no camp); 24 rows at most, as many as SET_PULL_LIST
// carries back. An answer to PULL_KINDS and to SET_PULL_LIST. The one
// exception to the rule that text is for people: a kind has no id of its
// own, so its name is the row's key, sent back in SET_PULL_LIST's names and
// kept in his list as written
typedef struct cl_pull_kind
{
    cl_header h;
    char      name[32];  // the kind's key: the name as the game holds it, underscores and all ("Forest_Hare"), or the race ("Goblins")
    uint8_t   conLow;    // the easiest and the toughest of them, the /check: 0 Too Weak .. 7 Incredibly Tough
    uint8_t   conHigh;
    uint8_t   count;     // how many stand round the camp
    uint8_t   on;        // 1 while his list has it on: his puller brings it in
    float     distance;  // their average distance from him, in yalms
} cl_pull_kind;

enum
{
    CL_PULL_REACH = 200, // yalms round the camp the page lists, and the puller picks within
};

// The Pull page's rows: each kind as a PULL_KIND answer (CL_F_MORE), the
// kinds on his list in its order first, then the others nearest first, then
// this. The con is read against `level`: the party's average level under
// Cardian's exp rule, else his own
typedef struct cl_pull_kinds
{
    cl_header h;
    uint8_t   count;  // answered: kinds sent
    uint8_t   level;  // answered: the level the con is read against
    uint8_t   staked; // answered: 1 when the kinds are round the camp, 0 round him (no camp)
    uint8_t   spare;
} cl_pull_kinds;

// What stands within CL_NEAR_REACH yalms of a member of his party, as the
// server has it, nearest her first: the list a far pick chooses from
// (OPEN_ISSUES #274, #500) -- the client knows only what stands near him,
// and keeps a monster's last place after it has gone -- asked again while
// the list is open, so it stays live. The kinds asked (CL_NEAR_*): the
// monsters her Attack would take -- alive, above ground, targetable,
// nobody's pet -- and the members of his party, herself and the fallen
// included, for an order aimed at a friend (the user, 2026-10-18). Each a
// MOB_NEAR (CL_F_MORE), CL_NEAR_MAX at most, then this; CL_S_OTHER_ZONE
// while she is not in his zone
typedef struct cl_mob_near
{
    cl_header h;
    uint16_t  targid;   // its index in the client's entity table: the order's target
    uint8_t   hpp;      // its HP, percent: 0 for a fallen member
    uint8_t   ally;     // 1: a member of his party; 0: a monster
    float     x;        // where it stands, in the server's axes (x, height, z)
    float     y;
    float     z;
    float     radius;   // its body's radius, yalms (the hitbox the game measures reach by)
    float     distance; // from her, yalms
    char      name[32]; // for people, as the game holds it ("Forest_Hare"): the client may not know it
} cl_mob_near;

typedef struct cl_mobs_near
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   kinds;   // asked: CL_NEAR_MONSTERS, CL_NEAR_ALLIES, or both
    uint8_t   count;   // answered: rows sent
    uint8_t   spare[2];
} cl_mobs_near;

enum
{
    CL_NEAR_MONSTERS = 1,  // the monsters near her
    CL_NEAR_ALLIES   = 2,  // the members of his party near her
    CL_NEAR_REACH    = 50, // yalms round her the list reaches: as far as his own target cursor reaches round him (measured, the user, 2026-10-18)
    CL_NEAR_MAX      = 24, // rows it carries at most
};

// His list as the page leaves it: the kinds in his order of preference, each
// on or off. Kinds of his list the page did not show (another camp's) are
// kept after them. The rows come back as they now stand (PULL_KIND,
// CL_F_MORE), then the outcome
typedef struct cl_set_pull_list
{
    cl_header h;
    uint8_t   count;      // rows sent, at most 24
    uint8_t   spare[3];
    uint8_t   on[24];     // 1 on, 0 off, by row
    char      names[24][32];
} cl_set_pull_list;

enum
{
    CL_PULL_NONE     = 0, // off the seat, or the party not pulling at a camp: no badge
    CL_PULL_WAITING  = 1, // waiting for the party to be ready, or for a mob to stand within reach
    CL_PULL_OUT      = 2, // out after a mob
    CL_PULL_BRINGING = 3, // bringing it in
};

// One-way, whenever it changes, and answering QUEUES for an addon that has
// just bound: a puller's badge beside the party list, its state and what she
// pulls with -- an item (her ranged weapon, else her ammunition), or for a
// Bard the song
typedef struct cl_pull_state
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   state;   // CL_PULL_*
    uint8_t   spare;
    uint16_t  item;    // 0 for none
    uint16_t  spell;   // 0 for none
    uint16_t  spare2;
} cl_pull_state;

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
    CL_T_OFFER          = 0x0509,
    CL_T_OFFER_ANSWER   = 0x050A,
    CL_T_SUPPLIES       = 0x050B,
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
// could not get in reach of it, or what the game answered when she tried;
// refused by the game on its next step, with the
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
    uint16_t  wait;     // unused: 0 (nothing is let go for its timing)
    uint16_t  message;  // CL_NOTE_REFUSED: the game's battle message (MsgBasic)
    uint16_t  spare2;
    char      about[32]; // for people: the battle message's name (REFUSED), the target's (UNREACHED)
} cl_note;

// One command behind the first on a queue line
typedef struct cl_queue_next
{
    cl_action action;
    uint16_t  target; // the target's index in the zone; 0 when it is gone
    uint16_t  spare;
} cl_queue_next;

// One-way, to a player at every change of a queue line of his (and an answer
// to QUEUES): the commands a character has waiting, in the order they go --
// one of his cardians' orders, held behind what she is doing or through a
// pause, up to four (the first, and up to three behind it), or his own one
// command through a pause -- and, ahead of them, the order of his she is
// carrying out now (running: from when it fires until her action is over:
// the cast lands or is cut, the ability, weapon skill, shot or item's use is
// done) -- and above it all her enchanted-item lane (#297): the piece she has
// put on and waits out (mode 0) or is using (mode 1), CL_AK_ITEM by its id,
// beside her line and holding no place in it. The server names them; the
// addon words them.
typedef struct cl_queue
{
    cl_header     h;
    uint32_t      character; // charid: one of his cardians, or himself
    cl_action     action;    // the first; CL_AK_NONE: nothing waits now
    uint16_t      target;    // the target's index in the zone; 0 when it is gone
    uint8_t       more;      // how many of next[] follow the first
    uint8_t       spare;
    cl_queue_next next[3];   // the commands behind the first, in order
    cl_queue_next running;   // the order she is carrying out now; CL_AK_NONE: none
    cl_queue_next lane;      // her enchanted item under way; CL_AK_NONE: none
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

// What a question the server puts is about. The server names it; the addon
// words it.
enum
{
    CL_OFFER_PARTY_WARP    = 1, // a warp he has picked, not yet bought: bought with his party on a yes, called off on a no
    CL_OFFER_WARP_TOGETHER = 2, // his own warp, a scroll read or the spell cast, held before it starts: choice 1 takes the party (each member with a way home uses hers), 2 goes alone, 0 calls it off and keeps it
};

// A member's way home on a warp together's question: what she will use
enum
{
    CL_WAY_NONE   = 0, // nothing: she stays behind
    CL_WAY_SCROLL = 1, // an Instant Warp from her bag
    CL_WAY_SPELL  = 2, // her own Warp
    CL_WAY_RING   = 3, // a Warp Ring of hers, put on and used
    CL_WAY_CUDGEL = 4, // a Warp Cudgel of hers, put on and used
};

// A piece of gear a warp together's member could warp by: its charges left,
// and the seconds until its recast is over (the delay wearing it starts is
// waited out after)
typedef struct cl_offer_piece
{
    uint8_t  charges;
    uint8_t  spare;
    uint16_t readyIn;
} cl_offer_piece;

// A character a question concerns, and for a warp together her way home: the
// server's pick, and every way she has, which he may pick from instead
typedef struct cl_offer_member
{
    uint32_t       character; // charid
    uint8_t        way;       // CL_WAY_*: the server's pick
    uint8_t        ways;      // the ways she has, a bit for each (1 << CL_WAY_*); staying behind is always one
    uint8_t        barred;    // the ways she has but cannot use now, the same bits: her Warp short of MP, silenced or on its recast; a piece on its recast or out of charges
    uint8_t        spare;
    cl_offer_piece ring;      // her Warp Ring, when she has one she can wear
    cl_offer_piece cudgel;    // her Warp Cudgel, the same
} cl_offer_member;

// One-way, to the player: a question the server puts to him, which the addon
// asks on a screen of the Cardian menu. He answers with OFFER_ANSWER, by its id;
// unanswered for `seconds` of the simulation's time (a pause holds it), or once
// he leaves his zone, it lapses and counts as his no. One stands for him at a
// time. Sent again with status OFFER_GONE, the same id, when it lapses or is
// withdrawn before he answers: the addon takes it down.
typedef struct cl_offer
{
    cl_header       h;
    uint32_t        offer;      // its id, which his answer names
    uint8_t         kind;       // CL_OFFER_*
    uint8_t         count;      // members used
    uint16_t        seconds;    // how long it stands unanswered
    uint8_t         own;        // a warp together: his own way (CL_WAY_*)
    uint8_t         spare[3];
    cl_offer_member members[5]; // the characters it concerns: a warp's cardians
} cl_offer;

// His answer to an OFFER. Answered OK, or OFFER_GONE when it is no longer open.
typedef struct cl_offer_answer
{
    cl_header h;
    uint32_t  offer;
    uint8_t   choice;  // 0 no (his cancel); 1 yes, the first choice; 2 the second, for a kind that has one
    uint8_t   ways[5]; // a warp together: his pick of each member's way, in the offer's order (CL_WAY_*)
    uint8_t   spare[2];
} cl_offer_answer;

// What a cardian did about her conquest supplies, the scrolls she buys at a
// conquest guard (pawn/supplies.h). 1 and 2 were her asking to be taken to
// the conquest shop, or to her nation's consulate: she walks there herself
// now (pawn/town_errands.h), and they are not used again
enum
{
    CL_SUPPLIES_BOUGHT = 0, // she bought item for price conquest points
};

// One-way, to the player: what a cardian of his party did about her conquest
// supplies. The server names it; the addon words it -- a purchase as the
// conquest exchange's own line.
typedef struct cl_supplies
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   kind;    // CL_SUPPLIES_*
    uint8_t   spare[3];
    uint16_t  item;    // BOUGHT: the item
    uint16_t  price;   // BOUGHT: what it cost her, in conquest points
} cl_supplies;

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
    CL_PRESENCE_STANDING = 1, // standing elsewhere: a city or town his shout reached
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
    uint8_t   sjob;         // her support job, 0 for none
    uint8_t   slvl;
    char      zoneName[32]; // for people
    char      line[96];     // for people
} cl_shout_responder;

// The shout for a goal: sixteen to twenty adventurers in the cities and
// towns hear it and answer in their own time, nearest first (their revealMs
// rise with the zone lines between them and him), each a SHOUT_RESPONDER
// (CL_F_MORE), then this. The same
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

// A look at one who answered his shout, whom his contract holds, or who
// qualifies as his linkshell's recruit (pawn/club.h): her jobs,
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
    uint16_t  missions;  // the story missions she and he have completed together
    uint16_t  spare;
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
// only how many are listed and what the last sales paid -- but for the sell
// panel's starting price (AH_HISTORY's start), which is one gil under the
// cheapest listing of anyone else's when that is lower than the going rate
// (the user, 2026-10-15: the fishing venture's own undercut). Its Sell page
// lists for him alone: whoever's bag an item comes from, the listing is his.

enum
{
    CL_T_AH_SHELF    = 0x0801,
    CL_T_AH_HISTORY  = 0x0802,
    CL_T_AH_BID      = 0x0803,
    CL_T_AH_QUOTES   = 0x0804,
    CL_T_AH_LISTINGS = 0x0805,
    CL_T_AH_SELL     = 0x0806,
    CL_T_AH_CANCEL   = 0x0807,
    CL_T_AH_REPRICE  = 0x0808,
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
// the going rate, and its last ten sales, newest first. Asked by the buy panel
// and the sell panel, which starts its price at `start`.
typedef struct cl_ah_history
{
    cl_header  h;
    uint16_t   item;
    uint8_t    stack;     // 1: the stack form
    uint8_t    count;     // answered: sales used
    uint32_t   stock;     // answered
    uint32_t   going;     // answered
    uint32_t   start;     // answered: where a sale of his starts -- the going rate, or one gil under the cheapest
                          // listing of anyone else's when that is lower; 0 with neither
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

// An item in one form and the going rate its last sales make: a row's price
// on the Sell page
typedef struct cl_ah_quote
{
    uint16_t item;
    uint8_t  stack; // 1: the stack form
    uint8_t  spare;
    uint32_t going; // answered: the median of its last ten sales in this form; 0 when none has sold
} cl_ah_quote;

// The going rates of many items at once, a bag's worth for the Sell page's
// list: asked with each item and form, answered the same with each rate. The
// server keeps a rate a minute, so a page flipping between bags and sellers
// asks the auction house little
typedef struct cl_ah_quotes
{
    cl_header   h;
    uint8_t     count; // quotes used
    uint8_t     spare;
    uint16_t    spare2;
    cl_ah_quote quotes[48];
} cl_ah_quotes;

// One listing of his: on sale, or sold with its gil still waiting in his
// delivery box
typedef struct cl_ah_mine
{
    uint32_t listing; // its own number, for a cancel
    uint32_t price;   // on sale: his price; sold: what it sold for
    uint32_t date;    // Unix time: when it went up, or when it sold
    uint16_t item;
    uint8_t  stack;   // 1: the stack form
    uint8_t  sold;    // 1: sold
} cl_ah_mine;

// His sales status for the Sell page: his listings on sale in the order they
// went up, then each sold one whose gil waits in his delivery box, the newest
// first; and the auction house's fees, so the page can say what a listing
// costs before it goes up -- base + price x rate / 10000, at most maxFee, as
// the game's own listing charges it
typedef struct cl_ah_listings
{
    cl_header  h;
    uint8_t    count;      // answered: rows used
    uint8_t    open;       // answered: his listings on sale, every one, whether all fit in rows or not
    uint8_t    limit;      // answered: how many he may have on sale at once; 0 for no limit
    uint8_t    spare;
    uint32_t   baseSingle; // answered: a single's fee before its rate
    uint32_t   baseStack;  // answered: a stack's
    uint32_t   rateSingle; // answered: a single's rate on its price, in hundredths of a percent
    uint32_t   rateStack;  // answered: a stack's
    uint32_t   maxFee;     // answered
    cl_ah_mine rows[48];   // answered
} cl_ah_listings;

// A listing of his: one piece or one full stack at his price, from a bag of
// a character he manages who stands by the counter with him -- himself, an
// alt, a cardian he owns -- or from the shell's bank. It is his whoever held
// it: his slots, his fee, his name on it, and its gil to his delivery box.
// From the bank it comes out into his inventory first, as BANK_WITHDRAW
// takes a stack for him, and is listed from there; refused after that, it
// stays in his inventory (taken). Answered with the outcome and, listed, the
// fee and the listing's number
typedef struct cl_ah_sell
{
    cl_header h;
    uint32_t  holder;  // charid: himself, an alt or a cardian he owns; 0: the shell's bank
    uint32_t  price;
    uint32_t  fee;     // answered: the fee he paid
    uint32_t  listing; // answered: the listing's own number
    uint16_t  item;    // the item the page saw there: another there now is refused CL_S_ITEM_SLIPPED_AWAY
    uint8_t   loc;     // her bag, a container id: the inventory, a satchel, sack or case, a wardrobe (the bank: 0)
    uint8_t   slot;    // her bag's slot; the bank's slot, 1 to CL_BANK_SIZE
    uint8_t   stack;   // 1: a full stack
    uint8_t   taken;   // answered: 1 once it came out of the bank into his inventory, listed or not
    uint16_t  version; // the bank: its version the page saw, as BANK_WITHDRAW's
} cl_ah_sell;

// A listing of his on sale taken back into his inventory, as the game's own
// cancel; refused with his inventory full or a Rare one in it already, and
// by an auction counter only. Answered with the slot it landed in
typedef struct cl_ah_cancel
{
    cl_header h;
    uint32_t  listing; // its own number
    uint16_t  item;    // the item the page saw listed: another is refused CL_S_LISTING_GONE
    uint8_t   landed;  // answered: his inventory's slot
    uint8_t   spare;
} cl_ah_cancel;

// A listing of his on sale given a new price, as taking it down and putting
// it up again would: the fee at the new price paid again and its date fresh,
// so its time on sale starts over (the user, 2026-10-15), while the listing
// itself stays up under its own number and needs no room in his inventory.
// Its own price again changes nothing and costs nothing. By an auction
// counter only. Answered with the fee
typedef struct cl_ah_reprice
{
    cl_header h;
    uint32_t  listing; // its own number
    uint32_t  price;
    uint32_t  fee;     // answered: the fee he paid
    uint16_t  item;    // the item the page saw listed: another is refused CL_S_LISTING_GONE
    uint16_t  spare;
} cl_ah_reprice;

// ---- 0x09xx: the Mog House -------------------------------------------------
//
// A cardian cannot walk into the player's Mog House, but what the Mog House
// does for her is done while he stands in his own, or by a Nomad Moogle (the
// roster's CL_MEMBER_BY_MOG_HOUSE), for a cardian of his to manage nearby:
// anywhere in the city of his Mog House (else CL_S_NOT_IN_CITY), within 20
// yalms of the moogle (else CL_S_OTHER_ZONE or CL_S_FAR_FROM_MOOGLE). Her
// jobs change by the game's own job change (packet 0x100).

enum
{
    CL_T_JOB_CHANGE = 0x0901,
    CL_T_DBOX       = 0x0902,
};

// Change her main job, her support job, or both; 0 keeps the one she has.
// Answered with her roster line as she stands now (MEMBER, CL_F_MORE) when
// the change is made, then this
typedef struct cl_job_change
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   mainJob; // a job id, or 0
    uint8_t   subJob;  // a job id, or 0
    uint16_t  spare;
} cl_job_change;

// Her delivery box, by the game's own (utils/dboxutils), run on her as his
// client runs it for him: each request opens her box, does its one thing
// and closes it again. Box 1 is what was sent to her (Receive), box 2 what
// she sends (Send); a box shows eight slots, and what comes for her past
// them waits to be pulled in as slots free. The game's own rules hold: an
// item flagged so goes only to a character of the same account, if at all;
// a box with too much waiting takes no more. Answered with her inventory
// (INVENTORY, CL_F_MORE) and her status pane (MEMBER_STATS: her gil) after
// a take or a set -- a set from another bag first with that bag, her
// inventory and her bag list (BAGS), the stack moved -- then this, with her
// box's eight slots as their rows hold them: every answer once she is
// found, refusals included, so a refusal never shows her box emptied.
enum
{
    CL_DBOX_LOOK   = 0, // nothing but her box's slots
    CL_DBOX_TAKE   = 1, // Receive: the slot's item, or gil, into her inventory; Send: an unsent one back
    CL_DBOX_DROP   = 2, // Receive: the slot's item thrown away
    CL_DBOX_RETURN = 3, // Receive: the slot's item sent back to whoever sent it
    CL_DBOX_SET    = 4, // Send: qty of the stack in her inventory's invSlot, into the slot, for `to`
    CL_DBOX_SEND   = 5, // Send: the slot's item sent
    CL_DBOX_RECALL = 6, // Send: a sent item not yet received, called back
    CL_DBOX_CLEAR  = 7, // Send: a delivered item's slot cleared
};

// A slot's state
enum
{
    CL_DBOX_EMPTY     = 0,
    CL_DBOX_WAITING   = 1, // Receive: an item for her; Send: set, not sent
    CL_DBOX_SENT      = 2, // Send: on its way
    CL_DBOX_DELIVERED = 3, // Send: received at the other end
};

typedef struct cl_dbox_slot
{
    uint16_t item;     // 0: empty; 65535: gil
    uint8_t  state;    // CL_DBOX_*
    uint8_t  spare;
    uint32_t qty;      // a stack's count, or the gil
    char     name[16]; // who sent it (Receive), or whom it is for (Send)
} cl_dbox_slot;

typedef struct cl_dbox
{
    cl_header    h;
    uint32_t     cardian;  // charid
    uint8_t      box;      // 1 Receive, 2 Send
    uint8_t      action;   // CL_DBOX_*
    uint8_t      slot;     // the box's slot, 0 to 7
    uint8_t      invSlot;  // CL_DBOX_SET: the slot of her inventory it comes from
    uint32_t     qty;      // CL_DBOX_SET: how many
    uint32_t     to;       // CL_DBOX_SET: the charid it is for
    uint8_t      waiting;  // answered, Receive: what waits for her beyond the eight slots
    uint8_t      loc;      // CL_DBOX_SET: the bag invSlot is in, 0 her inventory; from another, the stack comes into her inventory first, by her own move
    uint8_t      spare[2];
    cl_dbox_slot slots[8]; // answered
} cl_dbox;

// ---- 0x0Axx: the linkshell -------------------------------------------------
//
// The player's club (RESEARCH §11.13, pawn/club.h), on the game's own items:
// the Linkshell he holds (bought new from a linkshell vendor and made into
// one), and the Linkpearls he makes from it. The characters of his own
// account and the cardians it owns are in it with his pearl or without; one
// of the world's adventurers is in it while she wears a pearl of his shell,
// and stays wild. The Linkshell page lists them and what each can be asked
// now, invites one, trades her a pearl he made -- to one of the world's in
// his party, the recruit -- or breaks hers, and sends one on an errand or
// calls her back (pawn/errands.h).

enum
{
    CL_T_CLUB         = 0x0A01,
    CL_T_CLUB_MEMBER  = 0x0A02,
    CL_T_CLUB_INVITE  = 0x0A03,
    CL_T_PEARL        = 0x0A04,
    CL_T_ERRANDS      = 0x0A05,
    CL_T_ERRAND_GOAL  = 0x0A06,
    CL_T_SEND_ERRAND  = 0x0A07,
    CL_T_CALL_BACK    = 0x0A08,
    CL_T_CLUB_RECRUIT = 0x0A09,

    CL_T_PROFESSIONS      = 0x0A0A,
    CL_T_PROFESSION       = 0x0A0B,
    CL_T_PROFESSION_TOOLS = 0x0A0C,
    CL_T_PROFESSION_TOOL  = 0x0A0D,
    CL_T_START_PROFESSION = 0x0A0E,
    CL_T_VENTURE_SPOTS    = 0x0A0F,
    CL_T_VENTURE_SPOT     = 0x0A10,
    CL_T_VENTURE_BAITS    = 0x0A11,
    CL_T_VENTURE_BAIT     = 0x0A12,
    CL_T_VENTURE_PAID     = 0x0A13,
    CL_T_VENTURE_REPORT   = 0x0A14,
    CL_T_VENTURE_ITEM     = 0x0A15,
    CL_T_VENTURE_NEWS     = 0x0A16,
    CL_T_PROFESSION_SLOT  = 0x0A17,
};

// What she is to him
enum
{
    CL_CLUB_ALT     = 0, // a character of his own account
    CL_CLUB_OWNED   = 1, // a cardian his account owns
    CL_CLUB_WILD    = 2, // one of the world's adventurers wearing a pearl of his shell
    CL_CLUB_RECRUIT = 3, // one of the world's wearing no pearl whose affinity and missions with him reach the pearl's lock (pawn.PEARL_AFFINITY, PEARL_MISSIONS): not a member, he can recruit her
};

// A member's flags
enum
{
    CL_CLUB_PEARL    = 0x01, // she wears a pearl of his shell
    CL_CLUB_IN_PARTY = 0x02, // in his party
    CL_CLUB_STANDING = 0x04, // she has a body in the world
    CL_CLUB_ONLINE   = 0x08, // she is online: standing, or faded with search finding her
};

// The errands: what she is sent to do
enum
{
    CL_ERRAND_NONE  = 0,
    CL_ERRAND_GEAR  = 1, // gear up: the auction house's re-dress, and her scrolls from her nation's guard
    CL_ERRAND_QUEST = 2, // a quest he has done, from his log
    CL_ERRAND_LEVEL = 3, // a level on one of her jobs: reserved, not offered yet
    CL_ERRAND_MONEY = 4, // earning money at a profession of hers: a fishing venture, for now
    CL_ERRAND_RANK  = 5, // her nation's missions, to a rank he picks: at most his own
};

// Where an errand stands
enum
{
    CL_ERRAND_GOING = 1, // she is walking off: to a zone line, the auction house, the guard, and back
    CL_ERRAND_AWAY  = 2, // she is gone from the world, the errand under way
};

// What the page may do with a member now, a bit each: the server's word, so
// the page offers what works
enum
{
    CL_CAN_INVITE      = 0x01,
    CL_CAN_SEND_GEAR   = 0x02, // Gear up is hers to be sent on (now or once gearWhy clears)
    CL_CAN_SEND_QUEST  = 0x04,
    CL_CAN_CALL_BACK   = 0x08,
    CL_CAN_GIVE_PEARL  = 0x10, // she takes a pearl of his shell from him by the game's own trade (once he holds one and stands by her)
    CL_CAN_BREAK_PEARL = 0x20,
    CL_CAN_SEND_RANK   = 0x40, // her rank is below the catch-up's cap (rankCap)
};

// More of what the page may do with a member now: CLUB_MEMBER's can2
enum
{
    CL_CAN2_PROFESSIONS  = 0x01, // she may take up professions: an alt, or a cardian he owns
    CL_CAN2_REPORT       = 0x02, // she has a venture report: a money venture under way, its leftovers selling, or its last one done
    CL_CAN2_SEND_FISHING = 0x04, // her fishing is set up: a fishing venture is hers to be sent on
    CL_CAN2_HOMEPOINT    = 0x08, // she lies knocked out, in his party or not: HOMEPOINT takes her to his home point
};

// One member of his club, or one of the world's who qualifies to be his
// recruit: an answer to CLUB
typedef struct cl_club_member
{
    cl_header h;
    uint32_t  cardian;            // charid
    char      name[16];
    uint8_t   kind;               // CL_CLUB_ALT, _OWNED, _WILD or _RECRUIT
    uint8_t   flags;              // CL_CLUB_PEARL and the rest
    uint8_t   mainJob;
    uint8_t   mainLevel;
    uint8_t   subJob;
    uint8_t   subLevel;
    uint8_t   can;                // CL_CAN_*
    uint8_t   errand;             // CL_ERRAND_*: the errand she is on, CL_ERRAND_NONE for none
    uint8_t   errandState;        // CL_ERRAND_GOING or CL_ERRAND_AWAY
    uint8_t   nation;             // hers
    uint8_t   rank;               // hers, in her nation
    uint8_t   rankCap;            // the highest rank a catch-up takes her to now: his own, as far as the errand table goes
    uint16_t  gearWhy;            // CL_S_*: why Gear up cannot go now; 0 when it can
    uint16_t  zone;               // where she is: her body's zone, else where she was saved
    uint16_t  errandZone;         // away: the zone her errand has her crossing now; 0 for none
    uint16_t  homeIn;             // knocked out: seconds left of the game's hour before a fallen character is sent home; 0 otherwise
    uint32_t  secondsLeft;        // away: seconds on the game clock until she is back; 0 off the clock
    char      zoneName[32];       // for people
    char      errandZoneName[32]; // for people
    char      errandTitle[48];    // for people: the quest's title, "Rank 3"; empty for an errand the addon words
    uint8_t   levels[24];         // her level in each job, by job id (0 unused): her career
    uint32_t  secondsTotal;       // away on the clock: the errand's whole length, for its progress; 0 off the clock
    uint8_t   missionsDone;       // a rank catch-up away: the missions her time away has covered
    uint8_t   missions;           // a rank catch-up: the missions it takes her through
    uint16_t  affinity;           // one of the world's: her affinity with him (cardian_party_memory)
    uint16_t  missionsTogether;   // one of the world's: the story missions the two have completed together
    uint8_t   can2;               // CL_CAN2_*
    uint8_t   ventureDone;        // CL_PROF_*: her last venture's, over and its report unread since; 0 none
} cl_club_member;

// His club: each member a CLUB_MEMBER (CL_F_MORE), then each recruit, then
// this, with his shell. Holding none, the linkshell vendor of the city he
// stands in
typedef struct cl_club
{
    cl_header h;
    uint8_t   count;          // answered: members sent
    uint8_t   shell;          // answered: 1 while he holds a Linkshell or a Pearlsack
    uint8_t   pearls;         // answered: the Linkpearls of his shell in his inventory, to trade
    uint8_t   bank;           // answered: 1 while he stands where the linkshell bank opens (BANK)
    char      vendor[24];     // answered: holding no shell, in a city with a linkshell vendor: the vendor's name, for people
    char      vendorZone[32]; // answered: where the vendor stands, for people
    char      shellName[24];  // answered: the shell he wears (link slot 1, else 2), its name, for people; empty for none
    uint16_t  shellColor;     // answered: its colour, the game's own: four bits each of red, green, blue and alpha, red lowest
    uint8_t   rank;           // answered: CL_RANK_*: what he wears of it
    uint8_t   bankUsed;       // answered: the shell's bank's stacks
    uint32_t  bankGil;        // answered: the shell's bank's gil
} cl_club;

// The party invite he would send by hand, sent for him: the alts' rule of
// ROADMAP H -- in his zone she follows, in her own city she runs to him,
// anywhere else she holds until "follow me". Answered with the outcome
typedef struct cl_club_invite
{
    cl_header h;
    uint32_t  cardian; // charid
} cl_club_invite;

// Her pearl of his shell broken, as the game's holder breaks a member's
// pearl: the pearl is gone, and one of the world's goes back to the wild.
// A pearl is given by the game's own trade window, never by the Link: he
// trades her one, and the server plays her side (pawn/club.h)
typedef struct cl_pearl
{
    cl_header h;
    uint32_t  cardian; // charid
} cl_pearl;

// A quest he could send her on, or a rank she could be caught up to: an
// answer to ERRANDS. A quest: goal CL_GOAL_QUEST, its area and id. A rank:
// goal CL_GOAL_MISSION, her nation's log, the last mission it takes her
// through, and the rank
typedef struct cl_errand_goal
{
    cl_header h;
    uint8_t   goal;      // CL_GOAL_MISSION or CL_GOAL_QUEST
    uint8_t   log;       // the mission log, or the quest area
    uint16_t  id;        // the quest's id; a rank's last mission
    uint16_t  minutes;   // how long she is away doing it: minutes on the game clock (an Earth minute while unpaused)
    uint8_t   unlocks;   // 1: it unlocks a job, `job` (0: the support jobs)
    uint8_t   job;
    uint8_t   rank;      // a rank catch-up: the rank she reaches; 0 for a quest
    uint8_t   missions;  // a rank catch-up: how many missions it takes her through
    uint8_t   spare[2];
    char      title[48]; // for people
} cl_errand_goal;

// The quests of the errand table he has done and she has not, then the ranks
// she could be caught up to: each an ERRAND_GOAL (CL_F_MORE), then this
typedef struct cl_errands
{
    cl_header h;
    uint32_t  cardian; // charid: whom for
    uint8_t   count;   // answered
    uint8_t   spare[3];
} cl_errands;

// Send her on an errand. CL_ERRAND_QUEST names its quest (goal, log, id);
// CL_ERRAND_RANK the rank; CL_ERRAND_MONEY the profession, the spot (zone and
// area, one of VENTURE_SPOTS'), the rod (one of PROFESSION_TOOLS'), the bait
// (one of VENTURE_BAITS') and the hours; level and job are CL_ERRAND_LEVEL's,
// reserved. Answered with the outcome
typedef struct cl_send_errand
{
    cl_header h;
    uint32_t  cardian;    // charid
    uint8_t   errand;     // CL_ERRAND_*
    uint8_t   goal;       // CL_ERRAND_QUEST: CL_GOAL_QUEST
    uint8_t   log;
    uint8_t   level;
    uint16_t  id;
    uint8_t   job;
    uint8_t   rank;       // CL_ERRAND_RANK: the rank to reach
    uint8_t   profession; // CL_ERRAND_MONEY: CL_PROF_*
    uint8_t   hours;      // CL_ERRAND_MONEY: 1 or 2, hours of the game clock (Earth hours, held by a pause)
    uint16_t  zone;       // CL_ERRAND_MONEY: the spot's zone
    uint16_t  area;       // CL_ERRAND_MONEY: the spot's fishing area in the zone
    uint16_t  bait;       // CL_ERRAND_MONEY: the bait's item id
    uint16_t  rod;        // CL_ERRAND_MONEY: the rod's item id: one she holds, or a starter rod bought as it starts
    uint8_t   letItemsGo; // CL_ERRAND_MONEY: 1: an item that bites is let go, as a monster is
    uint8_t   endOnSnap;  // CL_ERRAND_MONEY: 1: a lost lure (a snapped line, or a broken rod that took it) ends the venture; 0: another replaces it
} cl_send_errand;

// Her errand ended early: what she has earned kept, and she comes back
typedef struct cl_call_back
{
    cl_header h;
    uint32_t  cardian; // charid
} cl_call_back;

// A recruit asked to join his linkshell. She thinks it over for decideMs and
// answers CL_S_OK, a yes in her words: one who qualifies never says no, and
// once the time is up she comes to him for the pearl, in no party. Any other
// outcome is the server's (CL_S_NOT_QUALIFIED, CL_S_NO_LINKSHELL,
// CL_S_NO_PEARL_TO_GIVE). The pearl itself he gives her with the game's own
// trade
typedef struct cl_club_recruit
{
    cl_header h;
    uint32_t  cardian;  // charid
    uint32_t  decideMs; // answered: how long she thinks it over
    char      line[96]; // answered: her words, for people
} cl_club_recruit;

// -- Professions and the money venture (RESEARCH §11.13, "Professions") --------------
// A profession is taken up on an alt or a cardian he owns from her Linkshell
// submenu; its ventures go under Assign Venture (SEND_ERRAND,
// CL_ERRAND_MONEY), each with its kit -- fishing: a rod and a bait, each one
// she holds or one bought as the venture starts, where it costs least (the
// auction house or a guild shop) -- and the kit sent becomes her
// profession's set. While one is under way the map fishes it on the game
// clock (pawn/angler.h), and pays from her purse, then from his
// (VENTURE_PAID).

// The professions, by the game's own skills
enum
{
    CL_PROF_NONE         = 0,
    CL_PROF_FISHING      = 1,
    CL_PROF_MINING       = 2,
    CL_PROF_LOGGING      = 3,
    CL_PROF_HARVESTING   = 4,
    CL_PROF_EXCAVATION   = 5,
    CL_PROF_WOODWORKING  = 6,
    CL_PROF_SMITHING     = 7,
    CL_PROF_GOLDSMITHING = 8,
    CL_PROF_CLOTHCRAFT   = 9,
    CL_PROF_LEATHERCRAFT = 10,
    CL_PROF_BONECRAFT    = 11,
    CL_PROF_ALCHEMY      = 12,
    CL_PROF_COOKING      = 13,
};

// Where a profession stands with her
enum
{
    CL_PROF_TAKEN  = 1, // she has taken it up: its ventures are offered
    CL_PROF_OPEN   = 2, // she may take it up now
    CL_PROF_LOCKED = 3, // not now: not built yet, or she has a profession already
};

// One profession, an answer to PROFESSIONS: every profession in CL_PROF_
// order, each with where it stands with her, and for one taken up its set:
// what she wears on its ventures, by the game's equipment slots (SLOT_MAIN 0
// to SLOT_BACK 15) -- fishing's rod in the ranged slot (2), its bait in the
// ammo slot (3) -- each slot an item id, 0 for empty. The set is what she was
// last sent with
typedef struct cl_profession
{
    cl_header h;
    uint8_t   profession; // CL_PROF_*
    uint8_t   state;      // CL_PROF_TAKEN, _OPEN or _LOCKED
    uint16_t  skill;      // her skill in it, in tenths of a level (the game's char_skills value)
    uint8_t   rank;       // her guild rank in it, the game's craft rank: 0 Amateur, 1 Recruit, 2 Initiate, 3 Novice ...
    uint8_t   letItemsGo; // taken: her last venture let items go (SEND_ERRAND's)
    uint8_t   endOnSnap;  // taken: her last venture's Line snap choice, 1 when a lost lure ended it (SEND_ERRAND's)
    uint8_t   spare;
    uint16_t  set[16];    // taken: her set, by equipment slot
} cl_profession;

// Her professions: each a PROFESSION (CL_F_MORE), then this
typedef struct cl_professions
{
    cl_header h;
    uint32_t  cardian;   // charid
    uint8_t   count;     // answered
    uint8_t   freeSlots;   // her open inventory spaces, where a venture's catch lands
    uint8_t   debugLength; // 1: SEND_ERRAND's hours 0 is offered, a one-minute venture that casts its hour at once (pawn.VENTURE_DEBUG)
    uint8_t   spare;
} cl_professions;

// What a profession's kit is
enum
{
    CL_TOOL_ROD  = 1, // a fishing rod
    CL_TOOL_BAIT = 2, // a bait or a lure
    CL_TOOL_GEAR = 3, // a piece of the profession's gear: fishing's, one with the game's fishing skill bonus
};

// Which kit PROFESSION_TOOLS asks for
enum
{
    CL_TOOLS_VENTURE = 1, // the rods a venture may take: the starter rods, bought where they cost least, and every rod she holds
    CL_TOOLS_SLOT    = 3, // what she holds that may go in one slot of her set: rods for the ranged slot, baits for the ammo slot, the profession's gear for the rest
};

// A piece of a profession's kit, an answer to PROFESSION_TOOLS
typedef struct cl_profession_tool
{
    cl_header h;
    uint16_t  item;    // the item id
    uint8_t   owned;   // how many she holds, to 255; 0: bought as the venture starts
    uint8_t   kind;    // CL_TOOL_*
    uint32_t  price;   // a venture's purchase of it (one rod, or a whole stack of bait), where it costs least now; 0 nobody sells it
} cl_profession_tool;

// A profession's kit: each a PROFESSION_TOOL (CL_F_MORE), then this.
// CL_TOOLS_VENTURE: the starter rods first, cheapest first, then those she
// holds (zone and area are not read)
typedef struct cl_profession_tools
{
    cl_header h;
    uint32_t  cardian;    // charid
    uint8_t   profession; // CL_PROF_*
    uint8_t   which;      // CL_TOOLS_*
    uint8_t   count;      // answered
    uint8_t   slot;       // CL_TOOLS_SLOT: the equipment slot
    uint16_t  zone;       // CL_TOOLS_VENTURE: the spot
    uint16_t  area;
} cl_profession_tools;

// One slot of her profession's set filled by hand, with something she holds
// that may go there (one of PROFESSION_TOOLS' CL_TOOLS_SLOT), or emptied
// (item 0). Answered with the outcome
typedef struct cl_profession_slot
{
    cl_header h;
    uint32_t  cardian;    // charid
    uint8_t   profession; // CL_PROF_*
    uint8_t   slot;       // the equipment slot, SLOT_MAIN 0 to SLOT_BACK 15
    uint16_t  item;       // the item id; 0 empties the slot
} cl_profession_slot;

// Take up a profession: its set starts empty, and fills from the kit she is
// sent with. Answered with the outcome
typedef struct cl_start_profession
{
    cl_header h;
    uint32_t  cardian;    // charid
    uint8_t   profession; // CL_PROF_*
    uint8_t   spare[3];
} cl_start_profession;

// A spot she may work at, an answer to VENTURE_SPOTS: fishing, each water
// (fishing area) of every zone she can walk to from where she stands at her
// level -- through zones whose usual monster is at her level or below
// (pawn/venture_spots.h; a stand-in until cardians travel, OPEN_ISSUES #431)
typedef struct cl_venture_spot
{
    cl_header h;
    uint16_t  zone;
    uint16_t  area;     // the fishing area she works, in the zone
    uint8_t   level;    // unused: 0
    uint8_t   hops;     // zone lines from where she stands
    uint16_t  spare2;
    char      name[48]; // the water's own name when its zone has several, for people ("Purgonorgo Isle - South Beach"); empty else
} cl_venture_spot;

// The spots of a profession of hers, nearest first: each a VENTURE_SPOT
// (CL_F_MORE), then this
typedef struct cl_venture_spots
{
    cl_header h;
    uint32_t  cardian;    // charid
    uint8_t   profession; // CL_PROF_*
    uint8_t   count;      // answered
    uint8_t   spare[2];
} cl_venture_spots;

// A bait for a spot with a rod, an answer to VENTURE_BAITS
typedef struct cl_venture_bait
{
    cl_header h;
    uint16_t  item;    // the bait's item id
    uint8_t   owned;   // how many she holds, to 255; 0: bought as the venture starts
    uint8_t   lure;    // 1: a lure: it does not stack, and the game keeps it through a catch (a snapped line or a broken rod takes it)
    uint32_t  price;   // a venture's purchase of it (a whole stack; a lure, one), where it costs least now; 0 nobody sells it
} cl_venture_bait;

// Every bait and lure of the game: each a VENTURE_BAIT (CL_F_MORE) -- what
// she holds, then what is for sale, cheapest first, then the rest -- then
// this
typedef struct cl_venture_baits
{
    cl_header h;
    uint32_t  cardian; // charid
    uint16_t  zone;
    uint16_t  area;
    uint16_t  rod;     // the rod chosen for the venture
    uint8_t   count;   // answered
    uint8_t   spare;
} cl_venture_baits;

// Pushed, one way: on a venture she bought a piece of her kit, from her
// purse alone or with his gil paying what hers was short. The addon words
// the line for his log, both purses' shares in it
typedef struct cl_venture_paid
{
    cl_header h;
    uint32_t  cardian;    // charid
    uint16_t  item;       // what she bought
    uint8_t   profession; // CL_PROF_*: whose venture
    uint8_t   spare;
    uint16_t  quantity;   // how many
    uint16_t  spare2;
    uint32_t  herGil;     // what her gil paid
    uint32_t  hisGil;     // what his gil paid
} cl_venture_paid;

// What a venture's news is
enum
{
    CL_NEWS_ROD_BROKE = 1, // her rod broke and she held no spare: the venture is cut short
    CL_NEWS_ROD_SPARE = 2, // her rod broke, and she goes on with a spare of it, and another of the lure the break took, if any
    CL_NEWS_BAGS_FULL = 3, // her inventory is full: the venture is cut short
    CL_NEWS_NO_GIL    = 4, // neither her gil nor his could buy more of the item (her bait): the venture is cut short
    CL_NEWS_NO_KIT    = 5, // she has none of the item left (her rod or her bait) and nobody sells it: the venture is cut short
    CL_NEWS_CANNOT_FISH = 6, // the server would not let her fish there: the venture is cut short
    CL_NEWS_LURE_LOST = 7,   // her lure (the item) was lost to a snapped line, and the venture ends, as the player chose
    CL_NEWS_ROD_LURE_LOST = 8, // her rod broke and took her lure, and the venture ends: a lost lure ends it, as the player chose, or she held no spare rod
    CL_NEWS_CAUGHT_UP = 9,     // the map was down while she was away, and the casts of that time were made up as it came back: minutes says how long
};

// Pushed, one way: news from her venture for his log
typedef struct cl_venture_news
{
    cl_header h;
    uint32_t  cardian; // charid
    uint8_t   news;    // CL_NEWS_*
    uint8_t   spare;
    uint16_t  item;    // what it is about: her rod, or her bait or lure
    uint16_t  lure;    // CL_NEWS_ROD_SPARE and CL_NEWS_ROD_LURE_LOST: the lure the broken rod took; 0 none
    uint16_t  minutes; // CL_NEWS_CAUGHT_UP: the minutes of her venture made up
} cl_venture_news;

// Where a thing of her trip is
enum
{
    CL_VENTURE_LISTED   = 1, // on the auction house, her listing
    CL_VENTURE_CARRIED  = 2, // in her bags, to be sold
    CL_VENTURE_SOLD     = 3, // sold: what it fetched
    CL_VENTURE_RETURNED = 4, // listed and sent home unsold, to her delivery box
    CL_VENTURE_CAUGHT   = 5, // caught on this venture, by kind: the count alone, wherever it is now
    CL_VENTURE_KEPT     = 6, // in her bags, out of her line: no vendor and no buyer would take it once the venture was over
    CL_VENTURE_USED     = 7, // a supply this venture used up, by kind: bait gone, rods broken, the count alone
    CL_VENTURE_BOUGHT   = 8, // a supply this venture bought, by kind: the count alone
};

// Where her venture stands
enum
{
    CL_VENTURE_NONE    = 0, // no venture to report
    CL_VENTURE_WORKING = 1, // away, working
    CL_VENTURE_SELLING = 2, // back: her leftovers selling
    CL_VENTURE_DONE    = 3, // back, all sold
};

// A thing of her venture, an answer to VENTURE_REPORT: what she caught, by
// item, each part of her catch by where it is now, and her supplies used up
// and bought, by item
typedef struct cl_venture_item
{
    cl_header h;
    uint16_t  item;     // the item id
    uint16_t  quantity;
    uint8_t   where;    // CL_VENTURE_*: LISTED, CARRIED, SOLD, RETURNED, KEPT, CAUGHT, USED or BOUGHT
    uint8_t   earlier;  // 1: left from an earlier venture, still her line to sell
    uint16_t  spare;
    uint32_t  value;    // sold: what it fetched; else what it should fetch, estimated
} cl_venture_item;

// Her last money venture, and nothing of an earlier one: each thing of it a
// VENTURE_ITEM (CL_F_MORE), then this with the venture's sums
typedef struct cl_venture_report
{
    cl_header h;
    uint32_t  cardian;     // charid
    uint8_t   profession;  // answered: CL_PROF_*
    uint8_t   phase;       // answered: CL_VENTURE_*
    uint8_t   count;       // answered: things sent
    uint8_t   spare;
    uint16_t  zone;        // answered: where she works or worked
    uint16_t  area;
    uint32_t  minutes;     // answered: how long the venture lasted, or has so far, on the game clock
    uint32_t  length;      // answered: the minutes it was sent for
    uint32_t  catches;     // answered: what she caught or found
    uint16_t  skillFrom;   // answered: her skill at the start, in tenths of a level
    uint16_t  skillTo;     // answered: her skill now
    int32_t   earned;      // answered: what its catch has fetched, less the auction house's fees
    uint32_t  pending;     // answered: what its catch still to sell should fetch, estimated
    uint32_t  supplies;    // answered: what it used up -- bait gone, rods broken -- at their prices
    uint32_t  spent;       // answered: the gil paid for its supplies, hers and his
    char      areaName[48]; // answered: the water's own name when its zone has several, for people; empty else
} cl_venture_report;

// ---- 0x0Bxx: the linkshell bank --------------------------------------------
//
// The shell's bank (pawn/ls_bank.h): CL_BANK_SIZE slots of items -- any
// item, Ex and Rare included -- and a balance of gil, one bank per
// linkshell, shared by everyone wearing a linkshell item of the shell (his
// link slot 1 names it, else slot 2), which nothing in the game reaches but
// these messages. Wearing none, each is refused CL_S_NO_SHELL_WORN. It opens
// while the player stands by an auction counter, by a Nomad Moogle or in his
// Mog House (CLUB's bank says so as the page reads it); anywhere else each
// of these messages is refused CL_S_BANK_CLOSED. What he may do follows his
// rank in the shell: the shell's holder anything, a sack holder and a pearl
// holder what the holder has set (BANK_RIGHTS), else CL_S_BANK_NOT_ALLOWED.
// Its stacks are kept merged and in order on every change -- by item, the
// fuller stack of one first -- so a bank slot names the stack at that place
// in the order as last sent; a move names the item and the slot the page
// saw, and finding another there is refused CL_S_ITEM_SLIPPED_AWAY (another
// player's move came first). Items move between the bank and a bag of a
// character he manages (a holder): himself, an alt of his account or a
// cardian it owns, standing in the world or not. A stack of one item merges
// with the bank's stacks of the same item and the same extra data (charges,
// augments, a pearl's shell, a signature), up to the item's stack size.

enum
{
    CL_T_BANK          = 0x0B01,
    CL_T_BANK_HOLDER   = 0x0B02,
    CL_T_BANK_DEPOSIT  = 0x0B03,
    CL_T_BANK_WITHDRAW = 0x0B04,
    CL_T_BANK_GIL      = 0x0B05,
    CL_T_BANK_RIGHTS   = 0x0B06,
    CL_T_BANK_JUNK     = 0x0B07,
};

enum { CL_BANK_SIZE = 200 };

// What a holder is to him
enum
{
    CL_HOLDER_SELF  = 0, // the player himself
    CL_HOLDER_ALT   = 1, // a character of his own account
    CL_HOLDER_OWNED = 2, // a cardian his account owns
};

// His rank in a shell, by the linkshell item he wears of it
enum
{
    CL_RANK_NONE   = 0, // he wears none
    CL_RANK_HOLDER = 1, // the shell itself: the founder's
    CL_RANK_SACK   = 2, // a Pearlsack: an officer
    CL_RANK_PEARL  = 3, // a Linkpearl
};

// What a rank may do in the bank: BANK's can, BANK_RIGHTS' bits
enum
{
    CL_BANK_PUT     = 0x01, // put items in
    CL_BANK_TAKE    = 0x02, // take items out
    CL_BANK_GIL_IN  = 0x04, // put gil in
    CL_BANK_GIL_OUT = 0x08, // take gil out
};

// A bag of a holder the bank reaches, and that bag as it stands: an answer to
// BANK (her inventory, loc 0), to a deposit or a withdrawal (the bag it
// changed), and to gil (loc 0). Asked by itself, for one holder and one of
// her bags (loc), it answers itself
typedef struct cl_bank_holder
{
    cl_header h;
    uint32_t  cardian;   // charid: the player himself, or one of his
    uint32_t  gil;       // answered: her gil
    char      name[16];  // answered
    uint8_t   kind;      // answered: CL_HOLDER_*
    uint8_t   loc;       // the bag: a container id, 0 the inventory; answered for that bag
    uint8_t   size;      // answered: the slots that bag has
    uint8_t   free;      // answered
    uint8_t   count;     // answered: items in use
    uint8_t   spare;
    uint16_t  why;       // answered: CL_S_*: why her bags cannot be reached now (away on a venture); 0 when they can
    uint8_t   sizes[18]; // answered: each of her bags' slots, by container id: 0 for a bag she has not, or that does not open here (the Mog House's outside it)
    uint16_t  spare2;
    cl_item   items[80]; // answered: that bag, as INVENTORY lists one
} cl_bank_holder;

// The bank as it stands. Asked, it answers each holder in his order -- the
// player himself first, then his alts and his own cardians by name -- as a
// BANK_HOLDER of her inventory (CL_F_MORE), then its stacks in parts of up to
// 80, each part a BANK, the last with the outcome. A move answers its stacks
// the same way (CL_F_MORE), ahead of its own outcome
typedef struct cl_bank
{
    cl_header h;
    uint8_t   holders;   // answered: holders sent ahead of the stacks (0 in a move's answer)
    uint8_t   used;      // answered: the bank's stacks, all parts together: 0 to CL_BANK_SIZE
    uint8_t   count;     // answered: stacks in this part
    uint8_t   rank;      // answered: his rank in the shell, CL_RANK_*
    uint8_t   can;       // answered: what his rank may do, CL_BANK_* bits
    uint8_t   sackCan;   // answered: what the holder lets a sack holder do
    uint8_t   pearlCan;  // answered: what the holder lets a pearl holder do
    uint8_t   spare;
    uint32_t  gil;       // answered: the bank's gil
    uint32_t  version;   // answered: changes with every move of its stacks, whoever made it (gil and the settings leave it)
    char      shell[24]; // answered: the shell's name, for people
    uint16_t  color;     // answered: the shell's colour, as CLUB's shellColor
    uint16_t  spare2;
    cl_item   items[80]; // answered: each stack with its bank slot, 1 to CL_BANK_SIZE, in the bank's order
} cl_bank;

// A stack from a holder's bag into the bank, whole or in part. Answered, once
// it has moved, by the holder's bag and the bank as they now stand
// (CL_F_MORE), then this; refused before anything moved, by this alone
typedef struct cl_bank_deposit
{
    cl_header h;
    uint32_t  cardian; // charid: the holder
    uint32_t  qty;
    uint8_t   slot;    // her bag's slot
    uint8_t   landed;  // answered: the bank slot of the first stack of that item now
    uint16_t  item;    // the item the page saw in the slot: another there now is refused CL_S_ITEM_SLIPPED_AWAY
    uint8_t   loc;     // her bag: a container id
    uint8_t   spare;
    uint16_t  spare2;
} cl_bank_deposit;

// A stack from the bank into a holder's bag, whole or in part, as the game
// gives an item: room in that bag, gear alone into a wardrobe, and no second
// of a Rare item. Answered as a deposit is
typedef struct cl_bank_withdraw
{
    cl_header h;
    uint32_t  cardian; // charid: the holder
    uint32_t  qty;
    uint8_t   slot;    // the bank slot, 1 to CL_BANK_SIZE
    uint8_t   landed;  // answered: her bag's slot it landed in
    uint16_t  item;    // the item the page saw in that bank slot: another there now is refused CL_S_ITEM_SLIPPED_AWAY
    uint8_t   loc;     // her bag: a container id
    uint8_t   spare;
    uint16_t  version; // the bank's version the page saw (cl_bank's), its low 16 bits: a bank moved since is refused
                       // CL_S_ITEM_SLIPPED_AWAY, so a stack its order put in that slot since is never taken for it
} cl_bank_withdraw;

// Gil between the bank and a holder's purse. Answered, once it has moved, by
// her inventory and the bank as they now stand (CL_F_MORE), then this with
// both balances; refused before anything moved (CL_S_NOT_ENOUGH_GIL: the
// purse it comes from is short), by this alone
typedef struct cl_bank_gil
{
    cl_header h;
    uint32_t  cardian; // charid: the holder
    uint32_t  amount;
    uint8_t   out;     // 1: from the bank to her; 0: from her to the bank
    uint8_t   loc;     // the bag of hers the page shows, which the answer brings back
    uint16_t  spare2;
    uint32_t  bankGil; // answered
    uint32_t  herGil;  // answered
} cl_bank_gil;

// What a sack holder and a pearl holder may do in the bank, CL_BANK_* bits:
// asked (set 0), answered as they stand; set (set 1) by the shell's holder
// alone, else refused CL_S_HOLDER_ONLY. The holder may always do everything
typedef struct cl_bank_rights
{
    cl_header h;
    uint8_t   set;
    uint8_t   sack;
    uint8_t   pearl;
    uint8_t   spare;
} cl_bank_rights;

// A holder's junk in one bag into the bank at once -- the player's word for
// it is haul: each stack there the junk rule names (pawn/junk.h: a raw
// material a vendor buys and no recipe makes), moved as BANK_DEPOSIT moves
// one, in her bag's order. A stack worn or busy stays; a bank too full for a
// stack goes on to the next, which may still merge. Answered, once it has
// all gone, by her bag and the bank as they now stand (CL_F_MORE), then this
// with what moved. Refused partway -- a deposit the bank or the database
// refused, which ends it -- this alone, its status the refusal and what moved
// before it in it. None moved: CL_S_NO_ITEM, none named there; CL_S_BANK_FULL,
// no room for any; the first stack's reason it stayed (CL_S_ITEM_BUSY, in a
// trade or a bazaar). Asked with look, it moves nothing and answers this
// alone, with what a press would move now, as the bank's room stands
typedef struct cl_bank_junk
{
    cl_header h;
    uint32_t  cardian;   // charid: the holder
    uint8_t   loc;       // her bag: a container id
    uint8_t   look;      // 1: only what it would move, nothing moved
    uint8_t   count;     // answered: stacks moved (or that would move)
    uint8_t   full;      // answered: 1 when the bank had no room for a stack it named
    cl_item   items[80]; // answered: each stack moved, by her bag's slot it left, in that order
} cl_bank_junk;

#pragma pack(pop)
