add_subdirectory(lua)
add_subdirectory(reporters)

# Catch2 hosts the C++ unit tests that run during xi_test startup, before the
# Lua test engine boots (see main.cpp).
suspend_clang_tidy()
CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.7.1
    SYSTEM ON
    OPTIONS
        "CATCH_INSTALL_DOCS OFF"
        "CATCH_INSTALL_EXTRAS OFF"
) # defines: Catch2::Catch2
resume_clang_tidy()

set(SOURCES
    ${LUA_SOURCES}
    ${REPORTERS_SOURCES}
    enums/test_status.h
    enums/tick_type.h
    tests/rng_spread_tests.cpp
    tests/utils/saturated_engine.h
    in_memory_sink.h
    mock_manager.cpp
    mock_manager.h
    mock_socket.h
    test_application.h
    test_application.cpp
    test_case.h
    test_case.cpp
    test_char.h
    test_char.cpp
    test_collector.h
    test_collector.cpp
    test_engine.h
    test_engine.cpp
    test_lua_environment.h
    test_lua_environment.cpp
    test_matcher.h
    test_matcher.cpp
    test_suite.h
    test_suite.cpp
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Apple")
    set_source_files_properties(
        tests/rng_spread_tests.cpp
        PROPERTIES
            COMPILE_OPTIONS "-fconstexpr-steps=2097152"
    )
endif()

# Catch2 test TUs must live in the executable, not xi_test_lib: their tests are
# registered by static initializers, which the linker drops for unreferenced
# object files in a static library.
xi_add_executable(xi_test testserver.rc
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/bitset_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/data_binding_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ecosystems_tree_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/merits_dataset_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/zones/mobs_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/zones/npcs_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/zones/regions_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/zones/settings_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/entity_list_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/item_container_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/rng_runtime_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/pathfind_region_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/roam_region_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_formation_tests.cpp # CARDIAN: formation arithmetic behind the link's position stream
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_danger_tests.cpp # CARDIAN: the danger map's radius rule
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_gambit_text_tests.cpp # CARDIAN: the gambit row grammar
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_rules_tests.cpp # CARDIAN: the rules a cardian fights by
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_planner_tests.cpp # CARDIAN: the local planner's grid and field
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_ladder_tests.cpp # CARDIAN: the seat ladder, who stands and who fades
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_tactics_tests.cpp # CARDIAN: the fight log's arithmetic (RESEARCH §12.5)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_conveyor_tests.cpp # CARDIAN: the conveyor's arithmetic (RESEARCH §12.12 item 2)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_perimeter_tests.cpp # CARDIAN: the perimeter's arithmetic (RESEARCH §12.15)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_stake_tests.cpp # CARDIAN: the stake's arithmetic (RESEARCH §12.16)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_rest_tests.cpp # CARDIAN: shared resting lifecycle and recovery arithmetic
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_cure_tests.cpp # CARDIAN: Cure readiness, emergency coordination and quiet-fight budgets
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_spell_movement_tests.cpp # CARDIAN: spell approach, sight recovery and sub-yalm navmesh paths
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_clock_tests.cpp # CARDIAN: the two clock domains (pause P1)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_pause_tests.cpp # CARDIAN: the combat pause's manager (pause P2)
    ${CMAKE_SOURCE_DIR}/src/map/pause/pause_module.cpp # CARDIAN: the pause's real module, so packet tests meet the real input gate (pause P3)
    ${CMAKE_SOURCE_DIR}/src/map/pawn/ls_bank_moves.cpp # CARDIAN: the bank's moves, for the cases of a failed move (#478)
    ${CMAKE_SOURCE_DIR}/src/map/pawn/ah_sell.cpp # CARDIAN: the Sell page's listing and cancel, for their cases
    ${CMAKE_SOURCE_DIR}/src/map/pawn/dbox.cpp # CARDIAN: a cardian's delivery box, for its cases
    ${CMAKE_SOURCE_DIR}/src/map/pawn/auction_purchase.cpp # CARDIAN: a cardian's bid with his purse behind hers, for its cases (#481)
    ${CMAKE_SOURCE_DIR}/src/map/pawn/item_moves.cpp # CARDIAN: a move kept whole when the database fails (#478), the bank's, the Sell page's and the bid's
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_calendar_tests.cpp # CARDIAN: the calendar's hold and drift (pause P6)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_gambit_defaults_tests.cpp # CARDIAN: the job default gambit rows and their ids
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_engage_tests.cpp # CARDIAN: the engage door's rules (ROADMAP K3)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_gambit_layers_tests.cpp # CARDIAN: the two layers of a character's gambit rows (ROADMAP K5)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_tactician_tests.cpp # CARDIAN: the tactician line's rules (ROADMAP K4)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_ailments_tests.cpp # CARDIAN: the Enfeeble status group and -na (best)'s pick
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_link_protocol_tests.cpp # CARDIAN: the Link's messages: sizes, headers, text
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_party_roles_tests.cpp # CARDIAN: the party's roles and the join rule (RESEARCH §17.6)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_tank_calls_tests.cpp # CARDIAN: the tank tactician's Provoke pacing
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_herd_tests.cpp # CARDIAN: the herd round a mob: spacing, order and the even step (herd_math.h)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_redress_tests.cpp # CARDIAN: a wild cardian re-dressed at the auction house: when to ask, the skill raise, the bag
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_warp_hold_tests.cpp # CARDIAN: the automatic hold a warp sets, and when a zone change is a walk
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_food_tests.cpp # CARDIAN: food: a script's facts, the stat a food gives, the owned pick and the moments she eats
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_finder_tests.cpp # CARDIAN: the party finder's rules: who hears, the yes or no, the seats and the order
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_claims_tests.cpp # CARDIAN: the party's claim board: who gives way, the slide and its purpose (claim_board.h)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_glance_tests.cpp # CARDIAN: an idle cardian's glances and emotes, and a mage's heading at her spot
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_rescue_tests.cpp # CARDIAN: the auto rescue's judgement
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_conquest_tests.cpp # CARDIAN: the conquest tally schedule and the crowd simulation sum (RESEARCH §20)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_errand_tests.cpp # CARDIAN: the linkshell errands: kinds, clock, route, call back, gear-up steps, her log
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_club_tests.cpp # CARDIAN: the linkshell club: its items, the vendors, the recruit on her way, the rank catch-up
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_fish_fight_tests.cpp # CARDIAN: a venture's fish fight, timed and not played
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_kit_loss_tests.cpp # CARDIAN: a venture cast's kit loss
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_route_tests.cpp # CARDIAN: a route searched across a whole zone
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_sneak_tests.cpp # CARDIAN: the sneak-by: a mob's reach over its rest, islands, the timed crossing
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_walk_tests.cpp # CARDIAN: the one walker's route arithmetic (ROADMAP W)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_junk_tests.cpp # CARDIAN: the junk rule (junk_math.h)
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_ls_bank_tests.cpp # CARDIAN: the linkshell bank tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_gambit_spells_tests.cpp # CARDIAN: the spells a gambit editor offers: only those she has learned
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_skillchain_tests.cpp # CARDIAN: skillchains and magic bursts: the options, the tactician's weapon skill, the TP clock, the burst
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_pull_tests.cpp # CARDIAN: the puller: the camp's gate, the con and the pull line, the tagging spot, the shot and the run, her kit
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_town_errand_tests.cpp # CARDIAN: the town errands: who goes, needs, stop order, lines, leaving the zone
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_riding_tests.cpp # CARDIAN: a cardian on a chocobo: the license and when she gets down
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_cohort_play_tests.cpp # CARDIAN: the cohorts in play and the town sign-in's arithmetic
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cardian_pawn_stubs.cpp # CARDIAN: the pawn module's side of upstream's marked calls, for a link without it
)
add_dependencies(xi_test data_schema_generate)

xi_add_library(xi_test_lib ${SOURCES})
target_include_directories(xi_test_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xi_test_lib
    PUBLIC
        xi_connect_lib
        xi_map_core
        xi_world_lib
        ${TEST_ONLY_EXTERNAL_LIBS}
)

target_link_libraries(xi_test PRIVATE xi_test_lib Catch2::Catch2)
xi_precompile_headers(xi_test)

if(TRACY_ENABLE)
    set_target_properties(xi_test PROPERTIES OUTPUT_NAME xi_test_tracy)
endif()
