#ifndef _DARKMUSHOPCURRENCY_H_
#define _DARKMUSHOPCURRENCY_H_

#pragma once

// DarkMu: personal shops trade in Zen or DarkCoins. The currency travels in bit 30
// of the stock 0x3F price field (both C2S SetItemPrice and S2C shop item lists), so
// no extra packet is needed and the value stays a positive 32-bit int.
// Server counterpart: DarkMu.Economy/PersonalShopCurrency.cs -- keep both in sync.

namespace DarkMuShop
{
    enum CURRENCY
    {
        CURRENCY_ZEN = 0,
        CURRENCY_DARKCOIN = 1,
    };

    constexpr unsigned int DARKCOIN_FLAG = 0x40000000u;
    constexpr int MAX_ZEN_PRICE = 0x3FFFFFFF;
    constexpr int MAX_DARKCOIN_PRICE = 100000000;
    constexpr int BUYER_FEE_PERCENT = 10;

    inline int GetCurrency(int wirePrice)
    {
        return (static_cast<unsigned int>(wirePrice) & DARKCOIN_FLAG) != 0 ? CURRENCY_DARKCOIN : CURRENCY_ZEN;
    }

    inline int GetAmount(int wirePrice)
    {
        return static_cast<int>(static_cast<unsigned int>(wirePrice) & ~DARKCOIN_FLAG);
    }

    inline bool IsDarkCoin(int wirePrice)
    {
        return GetCurrency(wirePrice) == CURRENCY_DARKCOIN;
    }

    inline int Encode(int amount, int currency)
    {
        if (currency != CURRENCY_DARKCOIN)
        {
            return amount;
        }

        return static_cast<int>(static_cast<unsigned int>(amount) | DARKCOIN_FLAG);
    }

    inline int GetMaxPrice(int currency)
    {
        return currency == CURRENCY_DARKCOIN ? MAX_DARKCOIN_PRICE : MAX_ZEN_PRICE;
    }

    /// Buyer-side surcharge on DarkCoin sales: 10% rounded up, at least 1.
    /// Mirrors EconomyDb.HoldBuyerPaymentAsync so the UI never understates the cost.
    inline int GetBuyerFee(int amount)
    {
        if (amount < 1)
        {
            return 0;
        }

        const int fee = (amount + BUYER_FEE_PERCENT - 1) / BUYER_FEE_PERCENT;
        return fee < 1 ? 1 : fee;
    }

    inline const wchar_t* GetCurrencyName(int currency)
    {
        return currency == CURRENCY_DARKCOIN ? L"DarkCoins" : L"Zen";
    }

    /// Currency picked for the next price dialog (set by the currency message box).
    inline int& PendingCurrency()
    {
        static int s_currency = CURRENCY_ZEN;
        return s_currency;
    }

    /// After inventory→shop drag, SetItemPrice must run once the item lands in the shop slot.
    inline int& PendingWirePrice()
    {
        static int s_wirePrice = 0;
        return s_wirePrice;
    }

    inline int& PendingShopSlot()
    {
        static int s_shopSlot = -1;
        return s_shopSlot;
    }

    inline void SetPendingShopPrice(int wirePrice, int shopSlot)
    {
        PendingWirePrice() = wirePrice;
        PendingShopSlot() = shopSlot;
    }

    inline void ClearPendingShopPrice()
    {
        PendingWirePrice() = 0;
        PendingShopSlot() = -1;
    }

    inline bool HasPendingShopPrice()
    {
        return PendingShopSlot() >= 0 && PendingWirePrice() != 0;
    }
}

#endif // _DARKMUSHOPCURRENCY_H_
