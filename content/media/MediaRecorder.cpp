/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaRecorder.h"
#include "GeneratedEvents.h"
#include "MediaEncoder.h"
#include "mozilla/Util.h"
#include "nsDOMEventTargetHelper.h"
#include "nsDOMFile.h"
#include "nsError.h"
#include "nsIDocument.h"
#include "nsIDOMBlobEvent.h"
#include "nsIDOMRecordErrorEvent.h"
#include "nsIScriptObjectPrincipal.h"
#include "nsIScriptSecurityManager.h"
#include "nsAString.h"
#include "nsTArray.h"

namespace mozilla {

namespace dom {

NS_IMPL_CYCLE_COLLECTION_INHERITED_2(MediaRecorder, nsDOMEventTargetHelper,
                                     mStream,
                                     mReadThread)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(MediaRecorder)
NS_INTERFACE_MAP_END_INHERITING(nsDOMEventTargetHelper)

NS_IMPL_ADDREF_INHERITED(MediaRecorder, nsDOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(MediaRecorder, nsDOMEventTargetHelper)

// This task is used for triggering the creating blob object and it runs at main thread.
class MediaRecorder::PushBlobTask : public nsRunnable
{
public:
  PushBlobTask(MediaRecorder* recorder)
    : mRecorder(recorder) {}

  NS_IMETHODIMP Run()
  {
    MOZ_ASSERT(NS_IsMainThread());
    nsresult rv = mRecorder->CreateAndDispatchBlobEvent();
    if (NS_FAILED(rv)) {
      mRecorder->NotifyError(rv);
    }
    return NS_OK;
  }
  MediaRecorder* mRecorder;
};

// This task is for firing the error message from encoder and it runs in main thread
class MediaRecorder::PushErrorMessageTask : public nsRunnable
{
public:
  PushErrorMessageTask(MediaRecorder* recorder, nsresult aError)
    : mRecorder(recorder),
      mError(aError) { }

  NS_IMETHODIMP Run()
  {
    MOZ_ASSERT(NS_IsMainThread());
    mRecorder->NotifyError(mError);
    return NS_OK;
  }

private:
  MediaRecorder* mRecorder;
  nsresult mError;
};

// This class is used for avoiding abort by directly use the NS_NewRunnableMethod and invoke ExtractEncodedData function
// The abort is checking if the destructor runs at main thread during the cycle-collect step in nsDOMEventTargetHelper
class MediaRecorder::ExtractEncodedDataTask : public nsRunnable
{
public:
  ExtractEncodedDataTask(MediaRecorder* aRecorder, MediaEncoder* aEncoder)
    : mRecorder(aRecorder),
      mEncoder(aEncoder) {}

  class ReleaseEncoderThreadTask : public nsRunnable
  {
  public:
    ReleaseEncoderThreadTask(already_AddRefed<MediaRecorder> recorder)
      : mRecorder(recorder) {}

    NS_IMETHODIMP Run()
    {
      MOZ_ASSERT(NS_IsMainThread());
      mRecorder->DispatchSimpleEvent(NS_LITERAL_STRING("stop"));
      mRecorder->mReadThread->Shutdown();
      mRecorder->mReadThread = nullptr;
      return NS_OK;
    }

  private:
    nsRefPtr<MediaRecorder> mRecorder;
  };

  NS_IMETHODIMP Run()
  {
    MOZ_ASSERT(!NS_IsMainThread());
    mRecorder->ExtractEncodedData();
    NS_DispatchToMainThread(new ReleaseEncoderThreadTask(mRecorder.forget()));
    return NS_OK;
  }

private:
  nsRefPtr<MediaRecorder> mRecorder;
  nsRefPtr<MediaEncoder>  mEncoder;
};

MediaRecorder::~MediaRecorder()
{
  if (mTrackUnionStream) {
    mTrackUnionStream->Destroy();
  }
}

void
MediaRecorder::Init(JSContext* aCx, nsPIDOMWindow* aOwnerWindow)
{
  MOZ_ASSERT(aOwnerWindow);
  MOZ_ASSERT(aOwnerWindow->IsInnerWindow());
  BindToOwner(aOwnerWindow);
}

MediaRecorder::MediaRecorder(DOMMediaStream& aStream)
  : mTimeSlice(0),
    mState(RecordingState::Inactive)
{
  mStream = &aStream;
  SetIsDOMBinding();
}

void
MediaRecorder::ExtractEncodedData()
{
  TimeStamp lastBlobTimeStamp = TimeStamp::Now();
  do {
    nsTArray<nsTArray<uint8_t> > outputBufs;
    mEncoder->GetEncodedData(&outputBufs, mMimeType);
    for (uint i = 0; i < outputBufs.Length(); i++) {
      mEncodedBufferCache->AppendBuffer(outputBufs[i]);
    }

    if (mTimeSlice > 0 && (TimeStamp::Now() - lastBlobTimeStamp).ToMilliseconds() > mTimeSlice) {
      NS_DispatchToMainThread(new PushBlobTask(this));
      lastBlobTimeStamp = TimeStamp::Now();
    }
  } while (mState == RecordingState::Recording && !mEncoder->IsShutdown());

  NS_DispatchToMainThread(new PushBlobTask(this));
}

void
MediaRecorder::Start(const Optional<int32_t>& aTimeSlice, ErrorResult& aResult)
{
  if (mState != RecordingState::Inactive) {
    aResult.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }

  if (aTimeSlice.WasPassed()) {
    if (aTimeSlice.Value() < 0) {
      aResult.Throw(NS_ERROR_INVALID_ARG);
      return;
    }
    mTimeSlice = aTimeSlice.Value();
  } else {
    mTimeSlice = 0;
  }

  // Create a TrackUnionStream to support Pause/Resume by using ChangeExplicitBlockerCount
  MediaStreamGraph* gm = mStream->GetStream()->Graph();
  mTrackUnionStream = gm->CreateTrackUnionStream(mStream);
  MOZ_ASSERT(mTrackUnionStream, "CreateTrackUnionStream failed");

  if (!CheckPrincipal()) {
    aResult.Throw(NS_ERROR_DOM_SECURITY_ERR);
    return;
  }

  if (mEncodedBufferCache == nullptr) {
    mEncodedBufferCache = new EncodedBufferCache(MAX_ALLOW_MEMORY_BUFFER);
  }

  mEncoder = MediaEncoder::CreateEncoder(NS_LITERAL_STRING(""));
  MOZ_ASSERT(mEncoder, "CreateEncoder failed");

  mTrackUnionStream->SetAutofinish(true);
  nsRefPtr<MediaInputPort> port =
    mTrackUnionStream->AllocateInputPort(mStream->GetStream(), MediaInputPort::FLAG_BLOCK_OUTPUT);

  if (mEncoder) {
    mTrackUnionStream->AddListener(mEncoder);
  } else {
    aResult.Throw(NS_ERROR_DOM_ABORT_ERR);
  }

  if (!mReadThread) {
    nsresult rv = NS_NewNamedThread("Media Encoder",
                                    getter_AddRefs(mReadThread));
    if (NS_FAILED(rv)) {
      aResult.Throw(rv);
      return;
    }
    nsRefPtr<ExtractEncodedDataTask> event = new ExtractEncodedDataTask(this, mEncoder);
    mReadThread->Dispatch(event, NS_DISPATCH_NORMAL);
    mState = RecordingState::Recording;
  }
}

void
MediaRecorder::Stop(ErrorResult& aResult)
{
  if (mState == RecordingState::Inactive) {
    aResult.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }
  mTrackUnionStream->RemoveListener(mEncoder);
  mState = RecordingState::Inactive;
}

void
MediaRecorder::Pause(ErrorResult& aResult)
{
  if (mState != RecordingState::Recording) {
    aResult.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }
  mTrackUnionStream->ChangeExplicitBlockerCount(-1);

  mState = RecordingState::Paused;
}

void
MediaRecorder::Resume(ErrorResult& aResult)
{
  if (mState != RecordingState::Paused) {
    aResult.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }
  mTrackUnionStream->ChangeExplicitBlockerCount(1);
  mState = RecordingState::Recording;
}

void
MediaRecorder::RequestData(ErrorResult& aResult)
{
  if (mState != RecordingState::Recording) {
    aResult.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }

  nsresult rv = CreateAndDispatchBlobEvent();
  if (NS_FAILED(rv)) {
    aResult.Throw(rv);
    return;
  }
}

JSObject*
MediaRecorder::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  return MediaRecorderBinding::Wrap(aCx, aScope, this);
}

/* static */ already_AddRefed<MediaRecorder>
MediaRecorder::Constructor(const GlobalObject& aGlobal, JSContext* aCx,
                           DOMMediaStream& aStream, ErrorResult& aRv)
{
  nsCOMPtr<nsIScriptGlobalObject> sgo = do_QueryInterface(aGlobal.Get());
  if (!sgo) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  nsCOMPtr<nsPIDOMWindow> ownerWindow = do_QueryInterface(aGlobal.Get());
  if (!ownerWindow) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  nsRefPtr<MediaRecorder> object = new MediaRecorder(aStream);
  object->Init(aCx, ownerWindow);
  return object.forget();
}

nsresult
MediaRecorder::CreateAndDispatchBlobEvent()
{
  NS_ABORT_IF_FALSE(NS_IsMainThread(), "Not running on main thread");

  if (!CheckPrincipal()) {
    // Media is not same-origin, don't allow the data out.
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  nsCOMPtr<nsIDOMBlob> blob;
  blob = mEncodedBufferCache->ExtractBlob(mMimeType);

  // create an event that uses the MessageEvent interface,
  // which does not bubble, is not cancelable, and has no default action
  nsCOMPtr<nsIDOMEvent> event;
  nsresult rv = NS_NewDOMBlobEvent(getter_AddRefs(event), this, nullptr, nullptr);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIDOMBlobEvent> blobEvent = do_QueryInterface(event);
  rv = blobEvent->InitBlobEvent(NS_LITERAL_STRING("dataavailable"),
    false, false, blob);
  NS_ENSURE_SUCCESS(rv, rv);

  event->SetTrusted(true);
  return DispatchDOMEvent(nullptr, event, nullptr, nullptr);
}

void
MediaRecorder::DispatchSimpleEvent(const nsAString & aStr)
{
  NS_ABORT_IF_FALSE(NS_IsMainThread(), "Not running on main thread");
  nsresult rv = CheckInnerWindowCorrectness();
  if (NS_FAILED(rv)) {
    return;
  }

  nsCOMPtr<nsIDOMEvent> event;
  rv = NS_NewDOMEvent(getter_AddRefs(event), this, nullptr, nullptr);
  if (NS_FAILED(rv)) {
    NS_WARNING("Failed to create the error event!!!");
    return;
  }
  rv = event->InitEvent(aStr, false, false);

  if (NS_FAILED(rv)) {
    NS_WARNING("Failed to init the error event!!!");
    return;
  }

  event->SetTrusted(true);

  rv = DispatchDOMEvent(nullptr, event, nullptr, nullptr);
  if (NS_FAILED(rv)) {
    NS_ERROR("Failed to dispatch the event!!!");
    return;
  }
}

void
MediaRecorder::NotifyError(nsresult aRv)
{
  NS_ABORT_IF_FALSE(NS_IsMainThread(), "Not running on main thread");
  nsresult rv = CheckInnerWindowCorrectness();
  if (NS_FAILED(rv)) {
    return;
  }
  nsString errorMsg;
  switch (aRv) {
  case NS_ERROR_DOM_SECURITY_ERR:
    errorMsg = NS_LITERAL_STRING("SecurityError");
    break;
  case NS_ERROR_OUT_OF_MEMORY:
    errorMsg = NS_LITERAL_STRING("OutOfMemoryError");
    break;
  default:
    errorMsg = NS_LITERAL_STRING("GenericError");
  }

  nsCOMPtr<nsIDOMEvent> event;
  rv = NS_NewDOMRecordErrorEvent(getter_AddRefs(event), this, nullptr, nullptr);

  nsCOMPtr<nsIDOMRecordErrorEvent> errorEvent = do_QueryInterface(event);
  rv = errorEvent->InitRecordErrorEvent(NS_LITERAL_STRING("error"),
                                        false, false, errorMsg);

  event->SetTrusted(true);
  rv = DispatchDOMEvent(nullptr, event, nullptr, nullptr);
  if (NS_FAILED(rv)) {
    NS_ERROR("Failed to dispatch the error event!!!");
    return;
  }
  return;
}

bool MediaRecorder::CheckPrincipal()
{
  nsCOMPtr<nsIPrincipal> principal = mStream->GetPrincipal();
  if (!GetOwner())
    return false;
  nsCOMPtr<nsIDocument> doc = GetOwner()->GetExtantDoc();
  if (!doc || !principal)
    return false;

  bool subsumes;
  if (NS_FAILED(doc->NodePrincipal()->Subsumes(principal, &subsumes)))
    return false;

  return subsumes;
}

}
}
