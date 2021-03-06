
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * A fake content node class so that the image frame for
 *   content: url(foo);
 * in CSS can have an nsIImageLoadingContent but use an
 * imgIRequest that's already been loaded from the style system.
 */

#include "nsContentCreatorFunctions.h"
#include "nsXMLElement.h"
#include "nsImageLoadingContent.h"
#include "imgIRequest.h"
#include "nsEventStates.h"

class nsGenConImageContent : public nsXMLElement,
                             public nsImageLoadingContent
{
public:
  nsGenConImageContent(already_AddRefed<nsINodeInfo> aNodeInfo)
    : nsXMLElement(aNodeInfo)
  {
    // nsImageLoadingContent starts out broken, so we start out
    // suppressed to match it.
    AddStatesSilently(NS_EVENT_STATE_SUPPRESSED);
  }

  nsresult Init(imgIRequest* aImageRequest)
  {
    // No need to notify, since we have no frame.
    return UseAsPrimaryRequest(aImageRequest, false);
  }

  // nsIContent overrides
  virtual nsEventStates IntrinsicState() const;
  
private:
  virtual ~nsGenConImageContent();

public:
  NS_DECL_ISUPPORTS_INHERITED
};

NS_IMPL_ISUPPORTS_INHERITED3(nsGenConImageContent, nsXMLElement,
                             nsIImageLoadingContent, imgIContainerObserver, imgIDecoderObserver)

nsresult
NS_NewGenConImageContent(nsIContent** aResult, already_AddRefed<nsINodeInfo> aNodeInfo,
                         imgIRequest* aImageRequest)
{
  NS_PRECONDITION(aImageRequest, "Must have request!");
  nsGenConImageContent *it = new nsGenConImageContent(aNodeInfo);
  if (!it)
    return NS_ERROR_OUT_OF_MEMORY;
  NS_ADDREF(*aResult = it);
  nsresult rv = it->Init(aImageRequest);
  if (NS_FAILED(rv))
    NS_RELEASE(*aResult);
  return rv;
}

nsGenConImageContent::~nsGenConImageContent()
{
  DestroyImageLoadingContent();
}

nsEventStates
nsGenConImageContent::IntrinsicState() const
{
  nsEventStates state = nsXMLElement::IntrinsicState();

  nsEventStates imageState = nsImageLoadingContent::ImageState();
  if (imageState.HasAtLeastOneOfStates(NS_EVENT_STATE_BROKEN | NS_EVENT_STATE_USERDISABLED)) {
    // We should never be in an error state; if the image fails to load, we
    // just go to the suppressed state.
    imageState |= NS_EVENT_STATE_SUPPRESSED;
    imageState &= ~NS_EVENT_STATE_BROKEN;
  }
  imageState &= ~NS_EVENT_STATE_LOADING;
  return state | imageState;
}
