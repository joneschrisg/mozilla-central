/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
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
 * The Original Code is Mozilla code.
 *
 * The Initial Developer of the Original Code is the Mozilla Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2007
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  Andreas Gal <gal@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
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

#include "mozilla/TimeStamp.h"
#include "nsTimeRanges.h"
#include "MediaResource.h"
#include "VideoUtils.h"
#include "nsMediaPluginReader.h"
#include "nsMediaPluginDecoder.h"
#include "nsMediaPluginHost.h"

using namespace mozilla;

nsMediaPluginReader::nsMediaPluginReader(nsBuiltinDecoder *aDecoder) :
  nsBuiltinDecoderReader(aDecoder),
  mPlugin(NULL),
  mVideoSeekTimeUs(-1),
  mAudioSeekTimeUs(-1),
  mLastVideoFrame(NULL)
{
  reinterpret_cast<nsMediaPluginDecoder *>(aDecoder)->GetContentType(mType);
}

nsMediaPluginReader::~nsMediaPluginReader()
{
  ResetDecode();
}

nsresult nsMediaPluginReader::Init(nsBuiltinDecoderReader* aCloneDonor)
{
  NS_ASSERTION(mDecoder->OnDecodeThread(), "Should be on decode thread.");

  return NS_OK;
}

nsresult nsMediaPluginReader::ReadMetadata(nsVideoInfo* aInfo)
{
  NS_ASSERTION(mDecoder->OnDecodeThread(), "Should be on decode thread.");

  if (!mPlugin) {
    mPlugin = GetMediaPluginHost()->CreateDecoder(mDecoder->GetResource(), mType);
    if (!mPlugin) {
      return NS_ERROR_FAILURE;
    }
  }

  // Set the total duration (the max of the audio and video track).
  int64_t durationUs;
  mPlugin->GetDuration(mPlugin, &durationUs);
  if (durationUs) {
    ReentrantMonitorAutoEnter mon(mDecoder->GetReentrantMonitor());
    mDecoder->GetStateMachine()->SetDuration(durationUs);
  }

  if (mPlugin->HasVideo(mPlugin)) {
    int32_t width, height;
    mPlugin->GetVideoParameters(mPlugin, &width, &height);
    nsIntRect pictureRect(0, 0, width, height);

    // Validate the container-reported frame and pictureRect sizes. This ensures
    // that our video frame creation code doesn't overflow.
    nsIntSize displaySize(width, height);
    nsIntSize frameSize(width, height);
    if (!nsVideoInfo::ValidateVideoRegion(frameSize, pictureRect, displaySize)) {
      return NS_ERROR_FAILURE;
    }

    // Video track's frame sizes will not overflow. Activate the video track.
    mHasVideo = mInfo.mHasVideo = true;
    mInfo.mDisplay = displaySize;
    mPicture = pictureRect;
    mInitialFrame = frameSize;
    VideoFrameContainer* container = mDecoder->GetVideoFrameContainer();
    if (container) {
      container->SetCurrentFrame(gfxIntSize(displaySize.width, displaySize.height),
                                 nsnull,
                                 mozilla::TimeStamp::Now());
    }
  }

  if (mPlugin->HasAudio(mPlugin)) {
    int32_t numChannels, sampleRate;
    mPlugin->GetAudioParameters(mPlugin, &numChannels, &sampleRate);
    mHasAudio = mInfo.mHasAudio = true;
    mInfo.mAudioChannels = numChannels;
    mInfo.mAudioRate = sampleRate;
  }

 *aInfo = mInfo;
  return NS_OK;
}

// Resets all state related to decoding, emptying all buffers etc.
nsresult nsMediaPluginReader::ResetDecode()
{
  if (mLastVideoFrame) {
    delete mLastVideoFrame;
    mLastVideoFrame = NULL;
  }
  if (mPlugin) {
    GetMediaPluginHost()->DestroyDecoder(mPlugin);
    mPlugin = NULL;
  }

  return NS_OK;
}

bool nsMediaPluginReader::DecodeVideoFrame(bool &aKeyframeSkip,
                                           PRInt64 aTimeThreshold)
{
  // Record number of frames decoded and parsed. Automatically update the
  // stats counters using the AutoNotifyDecoded stack-based class.
  PRUint32 parsed = 0, decoded = 0;
  nsMediaDecoder::AutoNotifyDecoded autoNotify(mDecoder, parsed, decoded);

  // Throw away the currently buffered frame if we are seeking.
  if (mLastVideoFrame && mVideoSeekTimeUs != -1) {
    delete mLastVideoFrame;
    mLastVideoFrame = NULL;
  }

  // Read next frame
  while (true) {
    MPAPI::VideoFrame frame;
    if (!mPlugin->ReadVideo(mPlugin, &frame, mVideoSeekTimeUs)) {
      // We reached the end of the video stream. If we have a buffered
      // video frame, push it the video queue using the total duration
      // of the video as the end time.
      if (mLastVideoFrame) {
        int64_t durationUs;
        mPlugin->GetDuration(mPlugin, &durationUs);
        mLastVideoFrame->mEndTime = (durationUs > mLastVideoFrame->mTime)
                                  ? durationUs
                                  : mLastVideoFrame->mTime;
        mVideoQueue.Push(mLastVideoFrame);
        mLastVideoFrame = NULL;
      }
      return false;
    }
    mVideoSeekTimeUs = -1;

    if (aKeyframeSkip) {
      if (!frame.mKeyFrame) {
        ++parsed;
        continue;
      }
      aKeyframeSkip = false;
    }

    VideoData::YCbCrBuffer b;
    b.mPlanes[0].mData = static_cast<PRUint8 *>(frame.Y.mData);
    b.mPlanes[0].mStride = frame.Y.mStride;
    b.mPlanes[0].mHeight = frame.Y.mHeight;
    b.mPlanes[0].mWidth = frame.Y.mWidth;
    b.mPlanes[0].mOffset = frame.Y.mOffset;
    b.mPlanes[0].mSkip = frame.Y.mSkip;

    b.mPlanes[1].mData = static_cast<PRUint8 *>(frame.Cb.mData);
    b.mPlanes[1].mStride = frame.Cb.mStride;
    b.mPlanes[1].mHeight = frame.Cb.mHeight;
    b.mPlanes[1].mWidth = frame.Cb.mWidth;
    b.mPlanes[1].mOffset = frame.Cb.mOffset;
    b.mPlanes[1].mSkip = frame.Cb.mSkip;

    b.mPlanes[2].mData = static_cast<PRUint8 *>(frame.Cr.mData);
    b.mPlanes[2].mStride = frame.Cr.mStride;
    b.mPlanes[2].mHeight = frame.Cr.mHeight;
    b.mPlanes[2].mWidth = frame.Cr.mWidth;
    b.mPlanes[2].mOffset = frame.Cr.mOffset;
    b.mPlanes[2].mSkip = frame.Cr.mSkip;

    nsIntRect picture = mPicture;
    if (frame.Y.mWidth != mInitialFrame.width ||
        frame.Y.mHeight != mInitialFrame.height) {

      // Frame size is different from what the container reports. This is legal,
      // and we will preserve the ratio of the crop rectangle as it
      // was reported relative to the picture size reported by the container.
      picture.x = (mPicture.x * frame.Y.mWidth) / mInitialFrame.width;
      picture.y = (mPicture.y * frame.Y.mHeight) / mInitialFrame.height;
      picture.width = (frame.Y.mWidth * mPicture.width) / mInitialFrame.width;
      picture.height = (frame.Y.mHeight * mPicture.height) / mInitialFrame.height;
    }

    // This is the approximate byte position in the stream.
    PRInt64 pos = mDecoder->GetResource()->Tell();

    VideoData *v = VideoData::Create(mInfo,
                                     mDecoder->GetImageContainer(),
                                     pos,
                                     frame.mTimeUs,
                                     frame.mTimeUs+1, // We don't know the end time.
                                     b,
                                     frame.mKeyFrame,
                                     -1,
                                     picture);

    if (!v) {
      return false;
    }
    parsed++;
    decoded++;
    NS_ASSERTION(decoded <= parsed, "Expect to decode fewer frames than parsed in MediaPlugin...");

    // Since MPAPI doesn't give us the end time of frames, we keep one frame
    // buffered in nsMediaPluginReader and push it into the queue as soon
    // we read the following frame so we can use that frame's start time as
    // the end time of the buffered frame.
    if (!mLastVideoFrame) {
      mLastVideoFrame = v;
      continue;
    }

    mLastVideoFrame->mEndTime = v->mTime;

    // We have the start time of the next frame, so we can push the previous
    // frame into the queue, except if the end time is below the threshold,
    // in which case it wouldn't be displayed anyway.
    if (mLastVideoFrame->mEndTime < aTimeThreshold) {
      delete mLastVideoFrame;
      mLastVideoFrame = NULL;
      continue;
    }

    mVideoQueue.Push(mLastVideoFrame);

    // Buffer the current frame we just decoded.
    mLastVideoFrame = v;

    break;
  }

  return true;
}

bool nsMediaPluginReader::DecodeAudioData()
{
  NS_ASSERTION(mDecoder->OnDecodeThread(), "Should be on decode thread.");

  // This is the approximate byte position in the stream.
  PRInt64 pos = mDecoder->GetResource()->Tell();

  // Read next frame
  MPAPI::AudioFrame frame;
  if (!mPlugin->ReadAudio(mPlugin, &frame, mAudioSeekTimeUs)) {
    return false;
  }
  mAudioSeekTimeUs = -1;

  nsAutoArrayPtr<AudioDataValue> buffer(new AudioDataValue[frame.mSize/2] );
  memcpy(buffer.get(), frame.mData, frame.mSize);

  PRUint32 frames = frame.mSize / (2 * frame.mAudioChannels);
  PRInt64 durationUs;
  if (!FramesToUsecs(frames, frame.mAudioSampleRate, durationUs))
    return false;

  mAudioQueue.Push(new AudioData(pos,
                                 frame.mTimeUs,
                                 durationUs,
                                 frames,
                                 buffer.forget(),
                                 frame.mAudioChannels));
  return true;
}

nsresult nsMediaPluginReader::Seek(PRInt64 aTarget, PRInt64 aStartTime, PRInt64 aEndTime, PRInt64 aCurrentTime)
{
  NS_ASSERTION(mDecoder->OnDecodeThread(), "Should be on decode thread.");

  mVideoQueue.Erase();
  mAudioQueue.Erase();

  mAudioSeekTimeUs = mVideoSeekTimeUs = aTarget;

  return DecodeToTarget(aTarget);
}

static uint64_t BytesToTime(int64_t offset, uint64_t length, uint64_t durationUs) {
  double perc = double(offset) / double(length);
  if (perc > 1.0)
    perc = 1.0;
  return uint64_t(double(durationUs) * perc);
}

nsresult nsMediaPluginReader::GetBuffered(nsTimeRanges* aBuffered, PRInt64 aStartTime)
{
  MediaResource* stream = mDecoder->GetResource();

  int64_t durationUs = 0;
  mPlugin->GetDuration(mPlugin, &durationUs);

  // Nothing to cache if the media takes 0us to play.
  if (!durationUs)
    return NS_OK;

  // Special case completely cached files.  This also handles local files.
  if (stream->IsDataCachedToEndOfResource(0)) {
    aBuffered->Add(0, durationUs);
    return NS_OK;
  }

  int64_t totalBytes = stream->GetLength();

  // If we can't determine the total size, pretend that we have nothing
  // buffered. This will put us in a state of eternally-low-on-undecoded-data
  // which is not get, but about the best we can do.
  if (totalBytes == -1)
    return NS_OK;

  PRInt64 startOffset = stream->GetNextCachedData(0);
  while (startOffset >= 0) {
    PRInt64 endOffset = stream->GetCachedDataEnd(startOffset);
    // Bytes [startOffset..endOffset] are cached.
    NS_ASSERTION(startOffset >= 0, "Integer underflow in GetBuffered");
    NS_ASSERTION(endOffset >= 0, "Integer underflow in GetBuffered");

    uint64_t startUs = BytesToTime(startOffset, totalBytes, durationUs);
    uint64_t endUs = BytesToTime(endOffset, totalBytes, durationUs);
    if (startUs != endUs) {
      aBuffered->Add(startUs, endUs);
    }
    startOffset = stream->GetNextCachedData(endOffset);
  }
  return NS_OK;
}
