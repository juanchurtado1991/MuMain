#pragma once

// Dark Rift — MuMain client side of C1 FE 01 [mask] (see deploy/client/dark-rift-ui/PROTOCOL.md).
// When active (Keeper), Devil Square enter UI shows +1..+7 labels and enables buttons by mask.
// Charon / stock DS leaves IsActive() false and uses original OpenningProcess logic.

#include <cstdint>

namespace DarkRiftClient
{
    void OnPacket(const unsigned char* buffer, int size);

    void SetActive(bool active);
    bool IsActive();

    // Bits 0..6 = tiers +1..+7
    std::uint8_t Mask();
    bool IsTierEnabled(int slotIndex); // slot 0..6

    void Clear();
}
