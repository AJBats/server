-----------------------------------
-- CARDIAN SETTINGS
-----------------------------------
-- Experimental client-fidelity switches for the Cardian project.
-----------------------------------

xi = xi or {}
xi.settings = xi.settings or {}

xi.settings.cardian =
{
    -- Tell a client that logs in solo its party structure (empty table, self
    -- row, self attrs), the triple LSB otherwise sends only on leaving a
    -- party. Corrects a "two of me" party window seen on the Cardian dev
    -- machine; cause unknown, unreported upstream. See login_party.cpp.
    SOLO_PARTY_AT_LOGIN = true,
}
