#pragma once

#include <cstdint>
#include <string>

#include "accounting/portfolio.h"
#include "exchange/paper_broker.h"

/*
-------------------------------------------
 reconcile_positions
-------------------------------------------

What a session's two accounts say about each other, which is the check Stage B is for rather than a
summary of the run. The broker's statement is built from the reports the venue sent; the portfolio is
built from the reports the strategy received. A fill the strategy never heard about is therefore a
position that disagrees with a statement saying it happened, and that disagreement is the only thing
this function has to find.

Only this strategy's own executions are compared, because the statement already filters to them: two
different populations would break on every venue print that was somebody else's.

Cash is deliberately not compared. The broker's USD arithmetic would be a second implementation of the
portfolio's instrument model, and a second implementation that agrees only because it was copied is not
a check; quantity, fill count and fees are quantities both sides count independently.

A fee is compared with a tolerance, because splitting one fill into two adds the parts in a different
order from the total they came from and the last bit can differ without anything being wrong.
-------------------------------------------
*/

struct ReconciliationSummary
{
    uint64_t symbols_checked{0};
    // Symbols whose position the two accounts disagree about, the largest single disagreement in
    // quantity units, and the signed sum of all of them. A dropped buy and a dropped sell can leave the
    // last one flat while two symbols disagree, which is why the count and the maximum sit next to it.
    uint64_t quantity_breaks{0};
    int64_t max_quantity_break{0};
    int64_t net_quantity_break{0};
    // Quantity traded on one side and not the other, over both directions. A lost fill that a lost fill
    // the other way happens to cancel leaves the position right and still shows here, which is the only
    // column that catches it: the position is what the run is worth, and this is what it did.
    uint64_t gross_quantity_break{0};
    uint64_t report_count_difference{0};
    double fee_difference{0.0};
    bool fees_match{true};

    bool broken() const
    {
        return quantity_breaks > 0 || gross_quantity_break > 0 || report_count_difference > 0 ||
               !fees_match;
    }

    std::string to_log_string() const;
};

ReconciliationSummary reconcile_positions(const BrokerStatement& statement, const Portfolio& books);
