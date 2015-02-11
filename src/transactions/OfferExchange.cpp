#include "OfferExchange.h"
#include "ledger/LedgerMaster.h"
#include "ledger/TrustFrame.h"
#include "database/Database.h"
#include "util/Logging.h"

namespace stellar
{

    OfferExchange::OfferExchange(LedgerDelta &delta, LedgerMaster &ledgerMaster) :
        mDelta(delta), mLedgerMaster(ledgerMaster)
    {

    }

    OfferExchange::CrossOfferResult OfferExchange::crossOffer(OfferFrame& sellingWheatOffer,
        int64_t maxWheatReceived, int64_t& numWheatReceived,
        int64_t maxSheepSend, int64_t& numSheepSend)
    {
        Currency& sheep = sellingWheatOffer.mEntry.offer().takerPays;
        Currency& wheat = sellingWheatOffer.mEntry.offer().takerGets;
        uint256& accountBID = sellingWheatOffer.mEntry.offer().accountID;

        Database &db = mLedgerMaster.getDatabase();

        AccountFrame accountB;
        if (!AccountFrame::loadAccount(accountBID, accountB, db))
        {
            throw std::runtime_error("invalid database state: offer must have matching account");
        }

        TrustFrame wheatLineAccountB;
        if (wheat.type() != NATIVE)
        {
            if (!TrustFrame::loadTrustLine(accountBID,
                wheat, wheatLineAccountB, db))
            {
                throw std::runtime_error("invalid database state: offer must have matching trust line");
            }
        }

        TrustFrame sheepLineAccountB;
        if (sheep.type() != NATIVE)
        {
            if (!TrustFrame::loadTrustLine(accountBID,
                sheep, sheepLineAccountB, db))
            {
                throw std::runtime_error("invalid database state: offer must have matching trust line");
            }
        }

        // what the seller has
        if (wheat.type() == NATIVE)
        {
            numWheatReceived = accountB.mEntry.account().balance;
        }
        else
        {
            numWheatReceived = wheatLineAccountB.mEntry.trustLine().balance;
        }

        // you can receive the lesser of the amount of wheat offered or
        // the amount the guy has

        if (numWheatReceived > maxWheatReceived)
        {
            numWheatReceived = maxWheatReceived;
        }

        if (numWheatReceived >= sellingWheatOffer.mEntry.offer().amount)
        {
            numWheatReceived = sellingWheatOffer.mEntry.offer().amount;
        }
        else
        {
            // update the offer based on the balance (to determine if it should be deleted or not)
            // note that we don't need to write into the db at this point as the actual update
            // is done further down
            sellingWheatOffer.mEntry.offer().amount = numWheatReceived;
        }

        // this guy can get X wheat to you. How many sheep does that get him?
        numSheepSend = bigDivide(
            numWheatReceived, sellingWheatOffer.mEntry.offer().price.n,
            sellingWheatOffer.mEntry.offer().price.d);

        if (numSheepSend > maxSheepSend)
        {
            // reduce the number even more if there is a limit on Sheep
            numSheepSend = maxSheepSend;
        }

        // bias towards seller
        numWheatReceived = bigDivide(
            numSheepSend, sellingWheatOffer.mEntry.offer().price.d,
            sellingWheatOffer.mEntry.offer().price.n);

        if (numWheatReceived == 0 || numSheepSend == 0)
        {
            // TODO: cleanup offers that result in this (offer amount too low to be converted)
            return eOfferCantConvert;
        }

        bool offerTaken = sellingWheatOffer.mEntry.offer().amount <= numWheatReceived;
        if (offerTaken)
        {   // entire offer is taken
            sellingWheatOffer.storeDelete(mDelta, db);
            accountB.mEntry.account().ownerCount--;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            sellingWheatOffer.mEntry.offer().amount -= numWheatReceived;
            sellingWheatOffer.storeChange(mDelta, db);
        }

        // Adjust balances
        if (sheep.type() == NATIVE)
        {
            accountB.mEntry.account().balance += numSheepSend;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            sheepLineAccountB.mEntry.trustLine().balance += numSheepSend;
            sheepLineAccountB.storeChange(mDelta, db);
        }

        if (wheat.type() == NATIVE)
        {
            accountB.mEntry.account().balance -= numWheatReceived;
            accountB.storeChange(mDelta, db);
        }
        else
        {
            wheatLineAccountB.mEntry.trustLine().balance -= numWheatReceived;
            wheatLineAccountB.storeChange(mDelta, db);
        }

        mOfferTrail.push_back(ClaimOfferAtom(
            accountB.getID(),
            sellingWheatOffer.getSequence(),
            wheat,
            numWheatReceived
            ));

        return offerTaken ? eOfferTaken : eOfferPartial;
    }

    OfferExchange::ConvertResult OfferExchange::convertWithOffers(
        Currency& sheep, int64_t maxSheepSend, int64_t &sheepSend,
        Currency& wheat, int64_t maxWheatReceive, int64_t &wheatReceived,
        std::function<OfferFilterResult(const OfferFrame &)> filter)
    {
        sheepSend = 0;
        wheatReceived = 0;

        Database &db = mLedgerMaster.getDatabase();

        size_t offerOffset = 0;

        bool needMore = (maxWheatReceive > 0 && maxSheepSend > 0);

        while (needMore)
        {
            std::vector<OfferFrame> retList;
            OfferFrame::loadBestOffers(5, offerOffset, sheep, wheat, retList, db);
            for (auto wheatOffer : retList)
            {
                if (filter)
                {
                    OfferFilterResult r = filter(wheatOffer);
                    switch (r)
                    {
                    case eKeep:
                        break;
                    case eStop:
                        return eFilterStop;
                    }
                }

                int64_t numWheatReceived;
                int64_t numSheepSend;

                CrossOfferResult cor = crossOffer(wheatOffer,
                    maxWheatReceive, numWheatReceived,
                    maxSheepSend, numSheepSend);

                switch (cor)
                {
                case eOfferTaken:
                    offerOffset--; // adjust offset as an offer was deleted
                    assert(offerOffset >= 0);
                    break;
                case eOfferPartial:
                    break;
                case eOfferCantConvert:
                    return ePartial;
                }

                sheepSend += numSheepSend;
                maxSheepSend -= numSheepSend;

                wheatReceived += numWheatReceived;
                maxWheatReceive -= numWheatReceived;

                needMore = (maxWheatReceive > 0 && maxSheepSend > 0);
                if (!needMore)
                {
                    return eOK;
                }
                else if (cor == eOfferPartial)
                {
                    return ePartial;
                }
            }

            // still stuff to fill but no more offers
            if (needMore && retList.size() < 5)
            {
                return eOK;
            }
            offerOffset += retList.size();
        }
        return eOK;
    }
}
