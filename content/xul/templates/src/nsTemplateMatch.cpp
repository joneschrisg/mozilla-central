/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsTemplateMatch.h"
#include "nsTemplateRule.h"

#ifdef NEED_CPP_UNUSED_IMPLEMENTATIONS
nsTemplateMatch::nsTemplateMatch(const nsTemplateMatch& aMatch) {}
void nsTemplateMatch::operator=(const nsTemplateMatch& aMatch) {}
#endif

// static
void
nsTemplateMatch::Destroy(nsFixedSizeAllocator& aPool,
                         nsTemplateMatch*& aMatch,
                         bool aRemoveResult) {
    if (aRemoveResult && aMatch->mResult)
        aMatch->mResult->HasBeenRemoved();
    aMatch->~nsTemplateMatch();
    aPool.Free(aMatch, sizeof(*aMatch));
    aMatch = nsnull;
}

nsresult
nsTemplateMatch::RuleMatched(nsTemplateQuerySet* aQuerySet,
                             nsTemplateRule* aRule,
                             PRInt16 aRuleIndex,
                             nsIXULTemplateResult* aResult)
{
    // assign the rule index, used to indicate that a match is active, and
    // so the tree builder can get the right action body to generate
    mRuleIndex = aRuleIndex;

    nsCOMPtr<nsIDOMNode> rulenode;
    aRule->GetRuleNode(getter_AddRefs(rulenode));
    if (rulenode)
        return aResult->RuleMatched(aQuerySet->mCompiledQuery, rulenode);

    return NS_OK;
}
