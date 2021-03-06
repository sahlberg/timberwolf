/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Travis Bogard <travis@netscape.com>
 *   HÂkan Waara <hwaara@chello.se>
 *   Mats Palmgren <matspal@gmail.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/*
 * rendering object for replaced elements that contain a document, such
 * as <frame>, <iframe>, and some <object>s
 */

#ifdef MOZ_IPC
#include "mozilla/layout/RenderFrameParent.h"
using mozilla::layout::RenderFrameParent;
#endif

#include "nsSubDocumentFrame.h"
#include "nsCOMPtr.h"
#include "nsGenericHTMLElement.h"
#include "nsIDocShell.h"
#include "nsIDocShellLoadInfo.h"
#include "nsIDocShellTreeItem.h"
#include "nsIDocShellTreeNode.h"
#include "nsIDocShellTreeOwner.h"
#include "nsIBaseWindow.h"
#include "nsIContentViewer.h"
#include "nsIDocumentViewer.h"
#include "nsPresContext.h"
#include "nsIPresShell.h"
#include "nsIComponentManager.h"
#include "nsFrameManager.h"
#include "nsIStreamListener.h"
#include "nsIURL.h"
#include "nsNetUtil.h"
#include "nsIDocument.h"
#include "nsIView.h"
#include "nsIViewManager.h"
#include "nsGkAtoms.h"
#include "nsStyleCoord.h"
#include "nsStyleContext.h"
#include "nsStyleConsts.h"
#include "nsFrameSetFrame.h"
#include "nsIDOMHTMLFrameElement.h"
#include "nsIDOMHTMLIFrameElement.h"
#include "nsIDOMXULElement.h"
#include "nsIScriptSecurityManager.h"
#include "nsXPIDLString.h"
#include "nsIScrollable.h"
#include "nsINameSpaceManager.h"
#include "nsWeakReference.h"
#include "nsIDOMWindow.h"
#include "nsIDOMDocument.h"
#include "nsIRenderingContext.h"
#include "nsIDOMNSHTMLDocument.h"
#include "nsDisplayList.h"
#include "nsUnicharUtils.h"
#include "nsIScrollableFrame.h"
#include "nsIObjectLoadingContent.h"
#include "nsLayoutUtils.h"
#include "FrameLayerBuilder.h"
#include "nsObjectFrame.h"

#ifdef MOZ_XUL
#include "nsXULPopupManager.h"
#endif

// For Accessibility
#ifdef ACCESSIBILITY
#include "nsAccessibilityService.h"
#endif
#include "nsIServiceManager.h"

using namespace mozilla;

static nsIDocument*
GetDocumentFromView(nsIView* aView)
{
  NS_PRECONDITION(aView, "");

  nsIFrame* f = static_cast<nsIFrame*>(aView->GetClientData());
  nsIPresShell* ps =  f ? f->PresContext()->PresShell() : nsnull;
  return ps ? ps->GetDocument() : nsnull;
}

class AsyncFrameInit;

nsSubDocumentFrame::nsSubDocumentFrame(nsStyleContext* aContext)
  : nsLeafFrame(aContext)
  , mIsInline(PR_FALSE)
  , mPostedReflowCallback(PR_FALSE)
  , mDidCreateDoc(PR_FALSE)
  , mCallingShow(PR_FALSE)
{
}

#ifdef ACCESSIBILITY
already_AddRefed<nsAccessible>
nsSubDocumentFrame::CreateAccessible()
{
  nsAccessibilityService* accService = nsIPresShell::AccService();
  return accService ?
    accService->CreateOuterDocAccessible(mContent, PresContext()->PresShell()) :
    nsnull;
}
#endif

NS_QUERYFRAME_HEAD(nsSubDocumentFrame)
  NS_QUERYFRAME_ENTRY(nsSubDocumentFrame)
NS_QUERYFRAME_TAIL_INHERITING(nsLeafFrame)

class AsyncFrameInit : public nsRunnable
{
public:
  AsyncFrameInit(nsIFrame* aFrame) : mFrame(aFrame) {}
  NS_IMETHOD Run()
  {
    if (mFrame.IsAlive()) {
      static_cast<nsSubDocumentFrame*>(mFrame.GetFrame())->ShowViewer();
    }
    return NS_OK;
  }
private:
  nsWeakFrame mFrame;
};

NS_IMETHODIMP
nsSubDocumentFrame::Init(nsIContent*     aContent,
                         nsIFrame*       aParent,
                         nsIFrame*       aPrevInFlow)
{
  // determine if we are a <frame> or <iframe>
  if (aContent) {
    nsCOMPtr<nsIDOMHTMLFrameElement> frameElem = do_QueryInterface(aContent);
    mIsInline = frameElem ? PR_FALSE : PR_TRUE;
  }

  nsresult rv =  nsLeafFrame::Init(aContent, aParent, aPrevInFlow);
  if (NS_FAILED(rv))
    return rv;

  // We are going to create an inner view.  If we need a view for the
  // OuterFrame but we wait for the normal view creation path in
  // nsCSSFrameConstructor, then we will lose because the inner view's
  // parent will already have been set to some outer view (e.g., the
  // canvas) when it really needs to have this frame's view as its
  // parent. So, create this frame's view right away, whether we
  // really need it or not, and the inner view will get it as the
  // parent.
  if (!HasView()) {
    rv = nsHTMLContainerFrame::CreateViewForFrame(this, PR_TRUE);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // Set the primary frame now so that
  // DocumentViewerImpl::FindContainerView called by ShowViewer below
  // can find it if necessary.
  aContent->SetPrimaryFrame(this);

  nsContentUtils::AddScriptRunner(new AsyncFrameInit(this));
  return NS_OK;
}

inline PRInt32 ConvertOverflow(PRUint8 aOverflow)
{
  switch (aOverflow) {
    case NS_STYLE_OVERFLOW_VISIBLE:
    case NS_STYLE_OVERFLOW_AUTO:
      return nsIScrollable::Scrollbar_Auto;
    case NS_STYLE_OVERFLOW_HIDDEN:
    case NS_STYLE_OVERFLOW_CLIP:
      return nsIScrollable::Scrollbar_Never;
    case NS_STYLE_OVERFLOW_SCROLL:
      return nsIScrollable::Scrollbar_Always;
  }
  NS_NOTREACHED("invalid overflow value passed to ConvertOverflow");
  return nsIScrollable::Scrollbar_Auto;
}

void
nsSubDocumentFrame::ShowViewer()
{
  if (mCallingShow) {
    return;
  }

  if (!PresContext()->IsDynamic()) {
    // We let the printing code take care of loading the document; just
    // create the inner view for it to use.
    (void) EnsureInnerView();
  } else {
    nsRefPtr<nsFrameLoader> frameloader = FrameLoader();
    if (frameloader) {
      nsIntSize margin = GetMarginAttributes();
      const nsStyleDisplay* disp = GetStyleDisplay();
      nsWeakFrame weakThis(this);
      mCallingShow = PR_TRUE;
      PRBool didCreateDoc =
        frameloader->Show(margin.width, margin.height,
                          ConvertOverflow(disp->mOverflowX),
                          ConvertOverflow(disp->mOverflowY),
                          this);
      if (!weakThis.IsAlive()) {
        return;
      }
      mCallingShow = PR_FALSE;
      mDidCreateDoc = didCreateDoc;
    }
  }
}

PRIntn
nsSubDocumentFrame::GetSkipSides() const
{
  return 0;
}

nsIFrame*
nsSubDocumentFrame::GetSubdocumentRootFrame()
{
  if (!mInnerView)
    return nsnull;
  nsIView* subdocView = mInnerView->GetFirstChild();
  if (!subdocView)
    return nsnull;
  return static_cast<nsIFrame*>(subdocView->GetClientData());
}

NS_IMETHODIMP
nsSubDocumentFrame::BuildDisplayList(nsDisplayListBuilder*   aBuilder,
                                     const nsRect&           aDirtyRect,
                                     const nsDisplayListSet& aLists)
{
  if (!IsVisibleForPainting(aBuilder))
    return NS_OK;

  if (aBuilder->IsForEventDelivery() &&
      GetStyleVisibility()->mPointerEvents == NS_STYLE_POINTER_EVENTS_NONE)
    return NS_OK;

  nsresult rv = DisplayBorderBackgroundOutline(aBuilder, aLists);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!mInnerView)
    return NS_OK;

#ifdef MOZ_IPC
  nsFrameLoader* frameLoader = FrameLoader();
  if (frameLoader) {
    RenderFrameParent* rfp = frameLoader->GetCurrentRemoteFrame();
    if (rfp) {
      return rfp->BuildDisplayList(aBuilder, this, aDirtyRect, aLists);
    }
  }
#endif

  nsIView* subdocView = mInnerView->GetFirstChild();
  if (!subdocView)
    return NS_OK;

  nsCOMPtr<nsIPresShell> presShell = nsnull;

  nsIFrame* subdocRootFrame =
    static_cast<nsIFrame*>(subdocView->GetClientData());

  if (subdocRootFrame) {
    presShell = subdocRootFrame->PresContext()->PresShell();
  }
  // If painting is suppressed in the presshell, we try to look for a better
  // presshell to use.
  if (!presShell || (presShell->IsPaintingSuppressed() &&
                     !aBuilder->IsIgnoringPaintSuppression())) {
    // During page transition mInnerView will sometimes have two children, the
    // first being the new page that may not have any frame, and the second
    // being the old page that will probably have a frame.
    nsIView* nextView = subdocView->GetNextSibling();
    nsIFrame* frame = nsnull;
    if (nextView) {
      frame = static_cast<nsIFrame*>(nextView->GetClientData());
    }
    if (frame) {
      nsIPresShell* ps = frame->PresContext()->PresShell();
      if (!presShell || (ps && !ps->IsPaintingSuppressed())) {
        subdocView = nextView;
        subdocRootFrame = frame;
        presShell = ps;
      }
    }
    if (!presShell) {
      // If we don't have a frame we use this roundabout way to get the pres shell.
      if (!mFrameLoader)
        return NS_OK;
      nsCOMPtr<nsIDocShell> docShell;
      mFrameLoader->GetDocShell(getter_AddRefs(docShell));
      if (!docShell)
        return NS_OK;
      docShell->GetPresShell(getter_AddRefs(presShell));
      if (!presShell)
        return NS_OK;
    }
  }

  nsPresContext* presContext = presShell->GetPresContext();

  nsDisplayList childItems;

  PRInt32 parentAPD = PresContext()->AppUnitsPerDevPixel();
  PRInt32 subdocAPD = presContext->AppUnitsPerDevPixel();

  nsIFrame* subdocRootScrollFrame = presShell->GetRootScrollFrame();

  nsRect dirty;
  if (subdocRootFrame) {
    if (presShell->UsingDisplayPort() && subdocRootScrollFrame) {
      dirty = presShell->GetDisplayPort();

      // The visual overflow rect of our viewport frame unfortunately may not
      // intersect with the displayport of that frame. For example, the scroll
      // offset of the frame may be (0, 0) so that the visual overflow rect
      // is (0, 0, 800px, 500px) while the display port may have its top-left
      // corner below y=500px.
      //
      // We have to force the frame to have a little faith and build a display
      // list anyway. (see nsIFrame::BuildDisplayListForChild for the short-
      // circuit code we are evading here).
      //
      subdocRootScrollFrame->AddStateBits(
        NS_FRAME_FORCE_DISPLAY_LIST_DESCEND_INTO);

    } else {
      // get the dirty rect relative to the root frame of the subdoc
      dirty = aDirtyRect + GetOffsetToCrossDoc(subdocRootFrame);
      // and convert into the appunits of the subdoc
      dirty = dirty.ConvertAppUnitsRoundOut(parentAPD, subdocAPD);
    }

    aBuilder->EnterPresShell(subdocRootFrame, dirty);
  }

  // The subdocView's bounds are in appunits of the subdocument, so adjust
  // them.
  nsRect subdocBoundsInParentUnits =
    subdocView->GetBounds().ConvertAppUnitsRoundOut(subdocAPD, parentAPD);

  // Get the bounds of subdocView relative to the reference frame.
  subdocBoundsInParentUnits = subdocBoundsInParentUnits +
                              mInnerView->GetPosition() +
                              GetOffsetToCrossDoc(aBuilder->ReferenceFrame());

  if (subdocRootFrame && NS_SUCCEEDED(rv)) {
    rv = subdocRootFrame->
           BuildDisplayListForStackingContext(aBuilder, dirty, &childItems);
  }

  if (!aBuilder->IsForEventDelivery()) {
    // If we are going to use a displayzoom below then any items we put under
    // it need to have underlying frames from the subdocument. So we need to
    // calculate the bounds based on which frame will be the underlying frame
    // for the canvas background color item.
    nsRect bounds;
    if (subdocRootFrame) {
      nsPoint offset = mInnerView->GetPosition() +
                       GetOffsetToCrossDoc(aBuilder->ReferenceFrame());
      offset = offset.ConvertAppUnits(parentAPD, subdocAPD);
      bounds = subdocView->GetBounds() + offset;
    } else {
      bounds = subdocBoundsInParentUnits;
    }

    // If we are in print preview/page layout we want to paint the grey
    // background behind the page, not the canvas color. The canvas color gets
    // painted on the page itself.
    if (nsLayoutUtils::NeedsPrintPreviewBackground(presContext)) {
      rv = presShell->AddPrintPreviewBackgroundItem(
             *aBuilder, childItems, subdocRootFrame ? subdocRootFrame : this,
             bounds);
    } else {
      // Add the canvas background color to the bottom of the list. This
      // happens after we've built the list so that AddCanvasBackgroundColorItem
      // can monkey with the contents if necessary.
      PRUint32 flags = nsIPresShell_MOZILLA_2_0_BRANCH::FORCE_DRAW;
      if (presContext->IsRootContentDocument()) {
        flags |= nsIPresShell_MOZILLA_2_0_BRANCH::ROOT_CONTENT_DOC_BG;
      }
      rv = presShell->AddCanvasBackgroundColorItem(
             *aBuilder, childItems, subdocRootFrame ? subdocRootFrame : this,
             bounds, NS_RGBA(0,0,0,0), flags);
    }
  }

  if (NS_SUCCEEDED(rv)) {

    bool addedLayer = false;

#ifdef MOZ_IPC
    // Make a scrollable layer in the child process so it can be manipulated
    // with transforms in the parent process.
    if (XRE_GetProcessType() == GeckoProcessType_Content) {
      nsIScrollableFrame* scrollFrame = presShell->GetRootScrollFrameAsScrollable();

      if (scrollFrame) {
        NS_ASSERTION(subdocRootFrame, "Root scroll frame should be non-null");
        nsRect scrollRange = scrollFrame->GetScrollRange();
        
        // Since making new layers is expensive, only use nsDisplayScrollLayer
        // if the area is scrollable.
        if (scrollRange.width != 0 || scrollRange.height != 0) {
          addedLayer = true;
          nsDisplayScrollLayer* layerItem = new (aBuilder) nsDisplayScrollLayer(
            aBuilder,
            &childItems,
            subdocRootScrollFrame,
            subdocRootFrame
          );
          childItems.AppendToTop(layerItem);
        }
      }
    }
#endif

    if (subdocRootFrame && parentAPD != subdocAPD) {
      NS_WARN_IF_FALSE(!addedLayer,
                       "Two container layers have been added. "
                       "Performance may suffer.");
      addedLayer = true;

      nsDisplayZoom* zoomItem =
        new (aBuilder) nsDisplayZoom(aBuilder, subdocRootFrame, &childItems,
                                     subdocAPD, parentAPD);
      childItems.AppendToTop(zoomItem);
    }
    
    if (!addedLayer && presContext->IsRootContentDocument()) {
      // We always want top level content documents to be in their own layer.
      nsDisplayOwnLayer* layerItem = new (aBuilder) nsDisplayOwnLayer(
        aBuilder, subdocRootFrame ? subdocRootFrame : this, &childItems);
      childItems.AppendToTop(layerItem);
    }

    // Clip children to the child root frame's rectangle
    rv = aLists.Content()->AppendNewToTop(
        new (aBuilder) nsDisplayClip(aBuilder, this, &childItems,
                                     subdocBoundsInParentUnits));
  }
  // delete childItems in case of OOM
  childItems.DeleteAll();

  if (subdocRootFrame) {
    aBuilder->LeavePresShell(subdocRootFrame, dirty);
  }

  return rv;
}

nscoord
nsSubDocumentFrame::GetIntrinsicWidth()
{
  if (!IsInline()) {
    return 0;  // HTML <frame> has no useful intrinsic width
  }

  if (mContent->IsXUL()) {
    return 0;  // XUL <iframe> and <browser> have no useful intrinsic width
  }

  NS_ASSERTION(ObtainIntrinsicSizeFrame() == nsnull,
               "Intrinsic width should come from the embedded document.");

  // We must be an HTML <iframe>.  Default to a width of 300, for IE
  // compat (and per CSS2.1 draft).
  return nsPresContext::CSSPixelsToAppUnits(300);
}

nscoord
nsSubDocumentFrame::GetIntrinsicHeight()
{
  // <frame> processing does not use this routine, only <iframe>
  NS_ASSERTION(IsInline(), "Shouldn't have been called");

  if (mContent->IsXUL()) {
    return 0;
  }

  NS_ASSERTION(ObtainIntrinsicSizeFrame() == nsnull,
               "Intrinsic height should come from the embedded document.");

  // Use 150px, for compatibility with IE, and per CSS2.1 draft.
  return nsPresContext::CSSPixelsToAppUnits(150);
}

#ifdef DEBUG
NS_IMETHODIMP nsSubDocumentFrame::GetFrameName(nsAString& aResult) const
{
  return MakeFrameName(NS_LITERAL_STRING("FrameOuter"), aResult);
}
#endif

nsIAtom*
nsSubDocumentFrame::GetType() const
{
  return nsGkAtoms::subDocumentFrame;
}

/* virtual */ nscoord
nsSubDocumentFrame::GetMinWidth(nsIRenderingContext *aRenderingContext)
{
  nscoord result;
  DISPLAY_MIN_WIDTH(this, result);

  nsIFrame* subDocRoot = ObtainIntrinsicSizeFrame();
  if (subDocRoot) {
    result = subDocRoot->GetMinWidth(aRenderingContext);
  } else {
    result = GetIntrinsicWidth();
  }

  return result;
}

/* virtual */ nscoord
nsSubDocumentFrame::GetPrefWidth(nsIRenderingContext *aRenderingContext)
{
  nscoord result;
  DISPLAY_PREF_WIDTH(this, result);

  nsIFrame* subDocRoot = ObtainIntrinsicSizeFrame();
  if (subDocRoot) {
    result = subDocRoot->GetPrefWidth(aRenderingContext);
  } else {
    result = GetIntrinsicWidth();
  }

  return result;
}

/* virtual */ nsIFrame::IntrinsicSize
nsSubDocumentFrame::GetIntrinsicSize()
{
  nsIFrame* subDocRoot = ObtainIntrinsicSizeFrame();
  if (subDocRoot) {
    return subDocRoot->GetIntrinsicSize();
  }
  return nsLeafFrame::GetIntrinsicSize();
}

/* virtual */ nsSize
nsSubDocumentFrame::GetIntrinsicRatio()
{
  nsIFrame* subDocRoot = ObtainIntrinsicSizeFrame();
  if (subDocRoot) {
    return subDocRoot->GetIntrinsicRatio();
  }
  return nsLeafFrame::GetIntrinsicRatio();
}

/* virtual */ nsSize
nsSubDocumentFrame::ComputeAutoSize(nsIRenderingContext *aRenderingContext,
                                    nsSize aCBSize, nscoord aAvailableWidth,
                                    nsSize aMargin, nsSize aBorder,
                                    nsSize aPadding, PRBool aShrinkWrap)
{
  if (!IsInline()) {
    return nsFrame::ComputeAutoSize(aRenderingContext, aCBSize,
                                    aAvailableWidth, aMargin, aBorder,
                                    aPadding, aShrinkWrap);
  }

  return nsLeafFrame::ComputeAutoSize(aRenderingContext, aCBSize,
                                      aAvailableWidth, aMargin, aBorder,
                                      aPadding, aShrinkWrap);  
}


/* virtual */ nsSize
nsSubDocumentFrame::ComputeSize(nsIRenderingContext *aRenderingContext,
                                nsSize aCBSize, nscoord aAvailableWidth,
                                nsSize aMargin, nsSize aBorder, nsSize aPadding,
                                PRBool aShrinkWrap)
{
  nsIFrame* subDocRoot = ObtainIntrinsicSizeFrame();
  if (subDocRoot) {
    return nsLayoutUtils::ComputeSizeWithIntrinsicDimensions(
                            aRenderingContext, this,
                            subDocRoot->GetIntrinsicSize(),
                            subDocRoot->GetIntrinsicRatio(),
                            aCBSize, aMargin, aBorder, aPadding);
  }
  return nsLeafFrame::ComputeSize(aRenderingContext, aCBSize, aAvailableWidth,
                                  aMargin, aBorder, aPadding, aShrinkWrap);
}

NS_IMETHODIMP
nsSubDocumentFrame::Reflow(nsPresContext*           aPresContext,
                           nsHTMLReflowMetrics&     aDesiredSize,
                           const nsHTMLReflowState& aReflowState,
                           nsReflowStatus&          aStatus)
{
  DO_GLOBAL_REFLOW_COUNT("nsSubDocumentFrame");
  DISPLAY_REFLOW(aPresContext, this, aReflowState, aDesiredSize, aStatus);
  // printf("OuterFrame::Reflow %X (%d,%d) \n", this, aReflowState.availableWidth, aReflowState.availableHeight);
  NS_FRAME_TRACE(NS_FRAME_TRACE_CALLS,
     ("enter nsSubDocumentFrame::Reflow: maxSize=%d,%d",
      aReflowState.availableWidth, aReflowState.availableHeight));

  aStatus = NS_FRAME_COMPLETE;

  NS_ASSERTION(mContent->GetPrimaryFrame() == this,
               "Shouldn't happen");

  // "offset" is the offset of our content area from our frame's
  // top-left corner.
  nsPoint offset(0, 0);
  
  if (IsInline()) {
    // XUL <iframe> or <browser>, or HTML <iframe>, <object> or <embed>
    nsresult rv = nsLeafFrame::DoReflow(aPresContext, aDesiredSize, aReflowState,
                                        aStatus);
    NS_ENSURE_SUCCESS(rv, rv);

    offset = nsPoint(aReflowState.mComputedBorderPadding.left,
                     aReflowState.mComputedBorderPadding.top);
  } else {
    // HTML <frame>
    SizeToAvailSize(aReflowState, aDesiredSize);
  }

  nsSize innerSize(aDesiredSize.width, aDesiredSize.height);
  if (IsInline()) {
    innerSize.width  -= aReflowState.mComputedBorderPadding.LeftRight();
    innerSize.height -= aReflowState.mComputedBorderPadding.TopBottom();
  }

  if (mInnerView) {
    nsIViewManager* vm = mInnerView->GetViewManager();
    vm->MoveViewTo(mInnerView, offset.x, offset.y);
    vm->ResizeView(mInnerView, nsRect(nsPoint(0, 0), innerSize), PR_TRUE);
  }

  // Determine if we need to repaint our border, background or outline
  CheckInvalidateSizeChange(aDesiredSize);

  FinishAndStoreOverflow(&aDesiredSize);

  if (!aPresContext->IsPaginated() && !mPostedReflowCallback) {
    PresContext()->PresShell()->PostReflowCallback(this);
    mPostedReflowCallback = PR_TRUE;
  }

  // printf("OuterFrame::Reflow DONE %X (%d,%d)\n", this,
  //        aDesiredSize.width, aDesiredSize.height);

  NS_FRAME_TRACE(NS_FRAME_TRACE_CALLS,
     ("exit nsSubDocumentFrame::Reflow: size=%d,%d status=%x",
      aDesiredSize.width, aDesiredSize.height, aStatus));

  NS_FRAME_SET_TRUNCATION(aStatus, aReflowState, aDesiredSize);
  return NS_OK;
}

PRBool
nsSubDocumentFrame::ReflowFinished()
{
  if (mFrameLoader) {
    nsWeakFrame weakFrame(this);

    mFrameLoader->UpdatePositionAndSize(this);

    if (weakFrame.IsAlive()) {
      // Make sure that we can post a reflow callback in the future.
      mPostedReflowCallback = PR_FALSE;
    }
  } else {
    mPostedReflowCallback = PR_FALSE;
  }
  return PR_FALSE;
}

void
nsSubDocumentFrame::ReflowCallbackCanceled()
{
  mPostedReflowCallback = PR_FALSE;
}

NS_IMETHODIMP
nsSubDocumentFrame::AttributeChanged(PRInt32 aNameSpaceID,
                                     nsIAtom* aAttribute,
                                     PRInt32 aModType)
{
  if (aNameSpaceID != kNameSpaceID_None) {
    return NS_OK;
  }
  
  // If the noResize attribute changes, dis/allow frame to be resized
  if (aAttribute == nsGkAtoms::noresize) {
    // Note that we're not doing content type checks, but that's ok -- if
    // they'd fail we will just end up with a null framesetFrame.
    if (mContent->GetParent()->Tag() == nsGkAtoms::frameset) {
      nsIFrame* parentFrame = GetParent();

      if (parentFrame) {
        // There is no interface for nsHTMLFramesetFrame so QI'ing to
        // concrete class, yay!
        nsHTMLFramesetFrame* framesetFrame = do_QueryFrame(parentFrame);
        if (framesetFrame) {
          framesetFrame->RecalculateBorderResize();
        }
      }
    }
  }
  else if (aAttribute == nsGkAtoms::showresizer) {
    nsIFrame* rootFrame = GetSubdocumentRootFrame();
    if (rootFrame) {
      rootFrame->PresContext()->PresShell()->
        FrameNeedsReflow(rootFrame, nsIPresShell::eResize, NS_FRAME_IS_DIRTY);
    }
  }
  else if (aAttribute == nsGkAtoms::type) {
    if (!mFrameLoader) 
      return NS_OK;

    if (!mContent->IsXUL()) {
      return NS_OK;
    }

    // Note: This logic duplicates a lot of logic in
    // nsFrameLoader::EnsureDocShell.  We should fix that.

    // Notify our enclosing chrome that our type has changed.  We only do this
    // if our parent is chrome, since in all other cases we're random content
    // subframes and the treeowner shouldn't worry about us.

    nsCOMPtr<nsIDocShell> docShell;
    mFrameLoader->GetDocShell(getter_AddRefs(docShell));
    nsCOMPtr<nsIDocShellTreeItem> docShellAsItem(do_QueryInterface(docShell));
    if (!docShellAsItem) {
      return NS_OK;
    }

    nsCOMPtr<nsIDocShellTreeItem> parentItem;
    docShellAsItem->GetParent(getter_AddRefs(parentItem));
    if (!parentItem) {
      return NS_OK;
    }

    PRInt32 parentType;
    parentItem->GetItemType(&parentType);

    if (parentType != nsIDocShellTreeItem::typeChrome) {
      return NS_OK;
    }

    nsCOMPtr<nsIDocShellTreeOwner> parentTreeOwner;
    parentItem->GetTreeOwner(getter_AddRefs(parentTreeOwner));
    if (parentTreeOwner) {
      nsAutoString value;
      mContent->GetAttr(kNameSpaceID_None, nsGkAtoms::type, value);

      PRBool is_primary = value.LowerCaseEqualsLiteral("content-primary");

#ifdef MOZ_XUL
      // when a content panel is no longer primary, hide any open popups it may have
      if (!is_primary) {
        nsXULPopupManager* pm = nsXULPopupManager::GetInstance();
        if (pm)
          pm->HidePopupsInDocShell(docShellAsItem);
      }
#endif

      parentTreeOwner->ContentShellRemoved(docShellAsItem);

      if (value.LowerCaseEqualsLiteral("content") ||
          StringBeginsWith(value, NS_LITERAL_STRING("content-"),
                           nsCaseInsensitiveStringComparator())) {
        PRBool is_targetable = is_primary ||
          value.LowerCaseEqualsLiteral("content-targetable");

        parentTreeOwner->ContentShellAdded(docShellAsItem, is_primary,
                                           is_targetable, value);
      }
    }
  }

  return NS_OK;
}

nsIFrame*
NS_NewSubDocumentFrame(nsIPresShell* aPresShell, nsStyleContext* aContext)
{
  return new (aPresShell) nsSubDocumentFrame(aContext);
}

NS_IMPL_FRAMEARENA_HELPERS(nsSubDocumentFrame)

void
nsSubDocumentFrame::DestroyFrom(nsIFrame* aDestructRoot)
{
  if (mPostedReflowCallback) {
    PresContext()->PresShell()->CancelReflowCallback(this);
    mPostedReflowCallback = PR_FALSE;
  }
  
  HideViewer();

  nsLeafFrame::DestroyFrom(aDestructRoot);
}

void
nsSubDocumentFrame::HideViewer()
{
  if (mFrameLoader && (mDidCreateDoc || mCallingShow))
    mFrameLoader->Hide();
}

nsIntSize
nsSubDocumentFrame::GetMarginAttributes()
{
  nsIntSize result(-1, -1);
  nsGenericHTMLElement *content = nsGenericHTMLElement::FromContent(mContent);
  if (content) {
    const nsAttrValue* attr = content->GetParsedAttr(nsGkAtoms::marginwidth);
    if (attr && attr->Type() == nsAttrValue::eInteger)
      result.width = attr->GetIntegerValue();
    attr = content->GetParsedAttr(nsGkAtoms::marginheight);
    if (attr && attr->Type() == nsAttrValue::eInteger)
      result.height = attr->GetIntegerValue();
  }
  return result;
}

nsFrameLoader*
nsSubDocumentFrame::FrameLoader()
{
  nsIContent* content = GetContent();
  if (!content)
    return nsnull;

  if (!mFrameLoader) {
    nsCOMPtr<nsIFrameLoaderOwner> loaderOwner = do_QueryInterface(content);
    if (loaderOwner) {
      nsCOMPtr<nsIFrameLoader> loader;
      loaderOwner->GetFrameLoader(getter_AddRefs(loader));
      mFrameLoader = static_cast<nsFrameLoader*>(loader.get());
    }
  }
  return mFrameLoader;
}

// XXX this should be called ObtainDocShell or something like that,
// to indicate that it could have side effects
nsresult
nsSubDocumentFrame::GetDocShell(nsIDocShell **aDocShell)
{
  *aDocShell = nsnull;

  NS_ENSURE_STATE(FrameLoader());
  return mFrameLoader->GetDocShell(aDocShell);
}

static void
DestroyDisplayItemDataForFrames(nsIFrame* aFrame)
{
  FrameLayerBuilder::DestroyDisplayItemDataFor(aFrame);

  PRInt32 listIndex = 0;
  nsIAtom* childList = nsnull;
  do {
    nsIFrame* child = aFrame->GetFirstChild(childList);
    while (child) {
      DestroyDisplayItemDataForFrames(child);
      child = child->GetNextSibling();
    }
    childList = aFrame->GetAdditionalChildListName(listIndex++);
  } while (childList);
}

static PRBool
BeginSwapDocShellsForDocument(nsIDocument* aDocument, void*)
{
  NS_PRECONDITION(aDocument, "");

  nsIPresShell* shell = aDocument->GetShell();
  nsIFrame* rootFrame = shell ? shell->GetRootFrame() : nsnull;
  if (rootFrame) {
    ::DestroyDisplayItemDataForFrames(rootFrame);
  }
  aDocument->EnumerateFreezableElements(
    nsObjectFrame::BeginSwapDocShells, nsnull);
  aDocument->EnumerateSubDocuments(BeginSwapDocShellsForDocument, nsnull);
  return PR_TRUE;
}

static nsIView*
BeginSwapDocShellsForViews(nsIView* aSibling)
{
  // Collect the removed sibling views in reverse order in 'removedViews'.
  nsIView* removedViews = nsnull;
  while (aSibling) {
    nsIDocument* doc = ::GetDocumentFromView(aSibling);
    if (doc) {
      ::BeginSwapDocShellsForDocument(doc, nsnull);
    }
    nsIView* next = aSibling->GetNextSibling();
    aSibling->GetViewManager()->RemoveChild(aSibling);
    aSibling->SetNextSibling(removedViews);
    removedViews = aSibling;
    aSibling = next;
  }
  return removedViews;
}

static void
InsertViewsInReverseOrder(nsIView* aSibling, nsIView* aParent)
{
  NS_PRECONDITION(aParent, "");
  NS_PRECONDITION(!aParent->GetFirstChild(), "inserting into non-empty list");

  nsIViewManager* vm = aParent->GetViewManager();
  while (aSibling) {
    nsIView* next = aSibling->GetNextSibling();
    aSibling->SetNextSibling(nsnull);
    // PR_TRUE means 'after' in document order which is 'before' in view order,
    // so this call prepends the child, thus reversing the siblings as we go.
    vm->InsertChild(aParent, aSibling, nsnull, PR_TRUE);
    aSibling = next;
  }
}

nsresult
nsSubDocumentFrame::BeginSwapDocShells(nsIFrame* aOther)
{
  if (!aOther || aOther->GetType() != nsGkAtoms::subDocumentFrame) {
    return NS_ERROR_NOT_IMPLEMENTED;
  }

  nsSubDocumentFrame* other = static_cast<nsSubDocumentFrame*>(aOther);
  if (!mFrameLoader || !mDidCreateDoc || mCallingShow ||
      !other->mFrameLoader || !other->mDidCreateDoc) {
    return NS_ERROR_NOT_IMPLEMENTED;
  }

  if (mInnerView && other->mInnerView) {
    nsIView* ourSubdocViews = mInnerView->GetFirstChild();
    nsIView* ourRemovedViews = ::BeginSwapDocShellsForViews(ourSubdocViews);
    nsIView* otherSubdocViews = other->mInnerView->GetFirstChild();
    nsIView* otherRemovedViews = ::BeginSwapDocShellsForViews(otherSubdocViews);

    ::InsertViewsInReverseOrder(ourRemovedViews, other->mInnerView);
    ::InsertViewsInReverseOrder(otherRemovedViews, mInnerView);
  }
  mFrameLoader.swap(other->mFrameLoader);
  return NS_OK;
}

static PRBool
EndSwapDocShellsForDocument(nsIDocument* aDocument, void*)
{
  NS_PRECONDITION(aDocument, "");

  // Our docshell and view trees have been updated for the new hierarchy.
  // Now also update all nsThebesDeviceContext::mWidget to that of the
  // container view in the new hierarchy.
  nsCOMPtr<nsISupports> container = aDocument->GetContainer();
  nsCOMPtr<nsIDocShell> ds = do_QueryInterface(container);
  if (ds) {
    nsCOMPtr<nsIContentViewer> cv;
    ds->GetContentViewer(getter_AddRefs(cv));
    while (cv) {
      nsCOMPtr<nsIDocumentViewer> dv = do_QueryInterface(cv);
      if (dv) {
        nsCOMPtr<nsPresContext> pc;
        dv->GetPresContext(getter_AddRefs(pc));
        nsIDeviceContext* dc = pc ? pc->DeviceContext() : nsnull;
        if (dc) {
          nsIView* v = dv->FindContainerView();
          dc->Init(v ? v->GetNearestWidget(nsnull) : nsnull);
        }
      }
      nsCOMPtr<nsIContentViewer> prev;
      cv->GetPreviousViewer(getter_AddRefs(prev));
      cv = prev;
    }
  }

  aDocument->EnumerateFreezableElements(
    nsObjectFrame::EndSwapDocShells, nsnull);
  aDocument->EnumerateSubDocuments(EndSwapDocShellsForDocument, nsnull);
  return PR_TRUE;
}

static void
EndSwapDocShellsForViews(nsIView* aSibling)
{
  for ( ; aSibling; aSibling = aSibling->GetNextSibling()) {
    nsIDocument* doc = ::GetDocumentFromView(aSibling);
    if (doc) {
      ::EndSwapDocShellsForDocument(doc, nsnull);
    }
  }
}

void
nsSubDocumentFrame::EndSwapDocShells(nsIFrame* aOther)
{
  nsSubDocumentFrame* other = static_cast<nsSubDocumentFrame*>(aOther);
  nsWeakFrame weakThis(this);
  nsWeakFrame weakOther(aOther);

  if (mInnerView) {
    ::EndSwapDocShellsForViews(mInnerView->GetFirstChild());
  }
  if (other->mInnerView) {
    ::EndSwapDocShellsForViews(other->mInnerView->GetFirstChild());
  }

  // Now make sure we reflow both frames, in case their contents
  // determine their size.
  // And repaint them, for good measure, in case there's nothing
  // interesting that happens during reflow.
  if (weakThis.IsAlive()) {
    PresContext()->PresShell()->
      FrameNeedsReflow(this, nsIPresShell::eTreeChange, NS_FRAME_IS_DIRTY);
    InvalidateFrameSubtree();
  }
  if (weakOther.IsAlive()) {
    other->PresContext()->PresShell()->
      FrameNeedsReflow(other, nsIPresShell::eTreeChange, NS_FRAME_IS_DIRTY);
    other->InvalidateFrameSubtree();
  }
}

nsIView*
nsSubDocumentFrame::EnsureInnerView()
{
  if (mInnerView) {
    return mInnerView;
  }

  // create, init, set the parent of the view
  nsIView* outerView = GetView();
  NS_ASSERTION(outerView, "Must have an outer view already");
  nsRect viewBounds(0, 0, 0, 0); // size will be fixed during reflow

  nsIViewManager* viewMan = outerView->GetViewManager();
  nsIView* innerView = viewMan->CreateView(viewBounds, outerView);
  if (!innerView) {
    NS_ERROR("Could not create inner view");
    return nsnull;
  }
  mInnerView = innerView;
  viewMan->InsertChild(outerView, innerView, nsnull, PR_TRUE);

  return mInnerView;
}

nsIFrame*
nsSubDocumentFrame::ObtainIntrinsicSizeFrame()
{
  nsCOMPtr<nsIObjectLoadingContent> olc = do_QueryInterface(GetContent());
  if (olc) {
    // We are an HTML <object>, <embed> or <applet> (a replaced element).

    // Try to get an nsIFrame for our sub-document's document element
    nsIFrame* subDocRoot = nsnull;

    nsCOMPtr<nsIDocShell> docShell;
    GetDocShell(getter_AddRefs(docShell));
    if (docShell) {
      nsCOMPtr<nsIPresShell> presShell;
      docShell->GetPresShell(getter_AddRefs(presShell));
      if (presShell) {
        nsIScrollableFrame* scrollable = presShell->GetRootScrollFrameAsScrollable();
        if (scrollable) {
          nsIFrame* scrolled = scrollable->GetScrolledFrame();
          if (scrolled) {
            subDocRoot = scrolled->GetFirstChild(nsnull);
          }
        }
      }
    }

#ifdef MOZ_SVG
    if (subDocRoot && subDocRoot->GetContent() &&
        subDocRoot->GetContent()->NodeInfo()->Equals(nsGkAtoms::svg, kNameSpaceID_SVG)) {
      return subDocRoot; // SVG documents have an intrinsic size
    }
#endif
  }
  return nsnull;
}
