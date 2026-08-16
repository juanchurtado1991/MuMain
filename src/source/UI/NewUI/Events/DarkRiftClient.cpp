#include "stdafx.h"
#include "UI/NewUI/Events/DarkRiftClient.h"
#include "UI/NewUI/NewUISystem.h"
#include "UI/NewUI/Events/NewUIEnterDevilSquare.h"

namespace DarkRiftClient
{
    namespace
    {
        volatile std::uint8_t g_mask = 0;
        volatile int g_active = 0;
    }

    void OnPacket(const unsigned char* buffer, int size)
    {
        // C1 05 FE 01 mm
        if (buffer == nullptr || size < 5)
        {
            return;
        }

        if (buffer[0] != 0xC1 && buffer[0] != 0xC3)
        {
            return;
        }

        if (buffer[2] == 0xFE && buffer[3] == 0x01)
        {
            g_mask = buffer[4];
            g_active = 1;
            g_ConsoleDebug->Write(MCD_RECEIVE,
                L"[DarkRift] eligibility mask=0x%02X", (unsigned)g_mask);

            // FE may arrive after the DS window already opened — refresh labels/locks.
            if (g_pNewUISystem && g_pNewUISystem->IsVisible(SEASON3B::INTERFACE_DEVILSQUARE)
                && g_pEnterDevilSquare)
            {
                g_pEnterDevilSquare->RefreshDarkRiftFromEligibility();
            }
        }
    }

    void SetActive(bool active)
    {
        g_active = active ? 1 : 0;
        if (!active)
        {
            g_mask = 0;
        }
    }

    bool IsActive()
    {
        return g_active != 0;
    }

    std::uint8_t Mask()
    {
        return g_mask;
    }

    bool IsTierEnabled(int slotIndex)
    {
        if (!IsActive() || slotIndex < 0 || slotIndex > 6)
        {
            return false;
        }
        return ((g_mask >> slotIndex) & 1) != 0;
    }

    void Clear()
    {
        g_active = 0;
        g_mask = 0;
    }
}
