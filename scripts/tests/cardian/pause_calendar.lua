-- The combat pause holds the calendar (pause P6): Vana'diel time stands still with the
-- simulation and carries on from where it stopped, however much real time went by.
-- The arithmetic is pinned in src/test/tests/cardian_calendar_tests.cpp; this is the
-- pause taking and letting go of it, and the game's own calendar functions following.
--
-- Real time going by is the harness's skipVanaDays: it moves the Earth clock by that
-- many game days. xi.cardian.pause is bound for tests by cardian_pawn_stubs.cpp.

describe('Combat pause: the calendar', function()
    local pause = xi.cardian.pause

    after_each(function()
        pause.release()
        pause.forgetDrift()
    end)

    it('stays on its day through a hold, and carries on from there', function()
        local day = VanadielUniqueDay()

        pause.hold()
        xi.test.world:skipVanaDays(3)
        assert(VanadielUniqueDay() == day, string.format('the calendar ran %d days in a held game', VanadielUniqueDay() - day))

        pause.release()
        assert(VanadielUniqueDay() == day, 'the calendar caught up at the release')

        xi.test.world:skipVanaDays(1)
        assert(VanadielUniqueDay() == day + 1, 'the calendar did not run on after the release')
    end)

    it('keeps the hour and the day of the week it was held at', function()
        xi.test.world:setVanaTime(11, 0)
        local weekday = VanadielDayOfTheWeek()

        pause.hold()
        xi.test.world:skipVanaDays(1)
        pause.release()

        assert(VanadielHour() == 11, string.format('held at 11:00, it is %d:00', VanadielHour()))
        assert(VanadielDayOfTheWeek() == weekday, 'the day of the week moved in a held game')
    end)
end)
