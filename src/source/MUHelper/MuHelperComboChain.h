#pragma once

// Set by MU Helper only while forcing the next combo step before the server
// 3s combo window expires (LetHeroStop + idle/SWORD bypass). Never leave true
// outside a single SimulateAttack call.
extern bool g_MuHelperComboForceChain;
