#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "EnumNames.h"
#include "MFTools.h"
#include "FrameGenerator.h"
#include "MediaStream.h"
#include "MediaSource.h"

namespace
{
	// Real, app-negotiated resolution (Phase 7): the vcam advertises multiple sizes and lets the
	// consuming app (Zoom/OBS/Chrome/...) pick one via normal MF/DirectShow negotiation, exactly
	// like a real UVC webcam -- there's no other mechanism, and forcing a live format change on a
	// running consumer would make the device disappear out from under it rather than gracefully
	// re-negotiate. Index 0 (1920x1080 RGB32) is kept first and unchanged from before this
	// existed, since some consumers pick index 0 blindly.
	//
	// 720p and 1080p only (not the vcam's job to invent sizes nobody asked for), landscape and
	// portrait for both (portrait is what lets a vertically-mounted phone + a tray rotation
	// setting fill the frame exactly -- see FrameGenerator's rotation math).
	constexpr UINT32 kSizes[][2] = { {1920, 1080}, {1280, 720}, {1080, 1920}, {720, 1280} };
	constexpr size_t kNumSizes = _countof(kSizes);

	HRESULT AddVideoType(IMFMediaType** out, REFGUID subtype, UINT32 width, UINT32 height)
	{
		wil::com_ptr_nothrow<IMFMediaType> type;
		RETURN_IF_FAILED(MFCreateMediaType(&type));
		type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		type->SetGUID(MF_MT_SUBTYPE, subtype);
		MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, width, height);
		type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
		type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
		MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, 30, 1);
		MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE_RANGE_MIN, 30, 1);
		MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE_RANGE_MAX, 30, 1);
		MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

		if (subtype == MFVideoFormat_RGB32)
		{
			type->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4);
			type->SetUINT32(MF_MT_SAMPLE_SIZE, width * height * 4);
			type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)((UINT64)width * height * 4 * 8 * 30));
		}
		else // NV12
		{
			// Y-plane stride -- NOT width*1.5. Setting this to the whole-frame byte multiplier
			// (the pre-Phase-7 bug) is evidence this type was never actually exercised by a real
			// consumer, since a wrong MF_MT_DEFAULT_STRIDE here would misread every NV12 sample.
			type->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
			type->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)((UINT64)width * height * 3 / 2));
			type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)((UINT64)width * height * 3 / 2 * 8 * 30));
		}

		*out = type.detach();
		return S_OK;
	}
}

HRESULT MediaStream::Initialize(IMFMediaSource* source, int index)
{
	RETURN_HR_IF_NULL(E_POINTER, source);
	_source = source;
	_index = index;

	RETURN_IF_FAILED(SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_STREAM_ID, index));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

	RETURN_IF_FAILED(MFCreateEventQueue(&_queue));

	// RGB32 then NV12, for each size in kSizes -- 8 types total. Must match what the phone can
	// actually produce (720p/1080p, from CaptureController.kt's own supported set) -- FrameGenerator
	// resamples to whatever a consumer picked, so there's no need for the ring's real content to
	// match exactly, just for these declared sizes to be ones FrameGenerator can actually fill.
	auto types = wil::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFMediaType>>(kNumSizes * 2);
	for (size_t i = 0; i < kNumSizes; ++i)
	{
		wil::com_ptr_nothrow<IMFMediaType> rgbType;
		RETURN_IF_FAILED(AddVideoType(&rgbType, MFVideoFormat_RGB32, kSizes[i][0], kSizes[i][1]));
		types[i * 2] = rgbType.detach();

		wil::com_ptr_nothrow<IMFMediaType> nv12Type;
		RETURN_IF_FAILED(AddVideoType(&nv12Type, MFVideoFormat_NV12, kSizes[i][0], kSizes[i][1]));
		types[i * 2 + 1] = nv12Type.detach();
	}

	RETURN_IF_FAILED_MSG(MFCreateStreamDescriptor(_index, (DWORD)types.size(), types.get(), &_descriptor), "MFCreateStreamDescriptor failed");

	wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
	RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
	TraceMFAttributes(handler.get(), L"MediaTypeHandler");
	RETURN_IF_FAILED(handler->SetCurrentMediaType(types[0]));

	return S_OK;
}

HRESULT MediaStream::Start(IMFMediaType* type)
{
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	if (type)
	{
		RETURN_IF_FAILED(type->GetGUID(MF_MT_SUBTYPE, &_format));
		WINTRACE(L"MediaStream::Start format: %s", GUID_ToStringW(_format).c_str());

		// Honor whatever size the consumer actually negotiated -- previously this was read and
		// then ignored, with EnsureRenderTarget always called at a hardcoded 1920x1080
		// regardless. Falls through to the last-known _negotiatedWidth/_negotiatedHeight (see
		// below) if this specific type has no MF_MT_FRAME_SIZE set, which shouldn't happen for
		// any type Initialize() itself declared, but is a safe no-op either way.
		UINT32 w = 0, h = 0;
		if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) && w && h)
		{
			_negotiatedWidth = w;
			_negotiatedHeight = h;
		}
	}
	// type == nullptr happens via SetStreamState(MF_STREAM_STATE_RUNNING) -- a real, exercised
	// path (e.g. a consumer that stops and restarts the stream without releasing the device) --
	// falls back to whatever was negotiated last, rather than a hardcoded default.
	WINTRACE(L"MediaStream::Start negotiated size: %ux%u", _negotiatedWidth, _negotiatedHeight);

	// at this point, set D3D manager may have not been called
	// so we want to create a D2D1 renter target anyway
	RETURN_IF_FAILED(_generator.EnsureRenderTarget(_negotiatedWidth, _negotiatedHeight));

	RETURN_IF_FAILED(_allocator->InitializeSampleAllocator(10, type));
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
	_state = MF_STREAM_STATE_RUNNING;
	return S_OK;
}

HRESULT MediaStream::Stop()
{
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	RETURN_IF_FAILED(_allocator->UninitializeSampleAllocator());
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
	_state = MF_STREAM_STATE_STOPPED;
	return S_OK;
}

MFSampleAllocatorUsage MediaStream::GetAllocatorUsage()
{
	return MFSampleAllocatorUsage_UsesProvidedAllocator;
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator)
{
	RETURN_HR_IF_NULL(E_POINTER, allocator);
	_allocator.reset();
	RETURN_HR(allocator->QueryInterface(&_allocator));
}

HRESULT MediaStream::SetD3DManager(IUnknown* manager)
{
	RETURN_HR_IF_NULL(E_POINTER, manager);

	// comment these 2 lines to force CPU usage
	RETURN_IF_FAILED(_allocator->SetDirectXManager(manager));
	// No size here anymore -- SetAllocator/SetD3DManager can both run before Start(), i.e. before
	// the negotiated MF_MT_FRAME_SIZE is known at all. _generator.EnsureRenderTarget(...), called
	// from Start() once that size IS known, is what actually sizes everything.
	RETURN_IF_FAILED(_generator.SetD3DManager(manager));
	return S_OK;
}

void MediaStream::Shutdown()
{
	if (_queue)
	{
		LOG_IF_FAILED_MSG(_queue->Shutdown(), "Queue shutdown failed");
		_queue.reset();
	}

	_descriptor.reset();
	_source.reset();
	_attributes.reset();
}

// IMFMediaEventGenerator
STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
	//WINTRACE(L"MediaSource::BeginGetEvent");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->BeginGetEvent(pCallback, punkState));
	return S_OK;
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
	//WINTRACE(L"MediaStream::EndGetEvent");
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->EndGetEvent(pResult, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
	WINTRACE(L"MediaStream::GetEvent");
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->GetEvent(dwFlags, ppEvent));
	return S_OK;
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
	WINTRACE(L"MediaStream::QueueEvent");
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);

	RETURN_IF_FAILED(_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue));
	return S_OK;
}

// IMFMediaStream
STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
	WINTRACE(L"MediaSource::GetMediaSource");
	RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
	*ppMediaSource = nullptr;
	RETURN_HR_IF(MF_E_SHUTDOWN, !_source);

	RETURN_IF_FAILED(_source.copy_to(ppMediaSource));
	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
	WINTRACE(L"MediaStream::GetStreamDescriptor");
	RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
	*ppStreamDescriptor = nullptr;
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);

	RETURN_IF_FAILED(_descriptor.copy_to(ppStreamDescriptor));
	return S_OK;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* pToken)
{
	//WINTRACE(L"MediaStream::RequestSample pToken:%p", pToken);
	winrt::slim_lock_guard lock(_lock);
	RETURN_HR_IF(MF_E_SHUTDOWN, !_allocator || !_queue);

	wil::com_ptr_nothrow<IMFSample> sample;
	RETURN_IF_FAILED(_allocator->AllocateSample(&sample));
	RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
	RETURN_IF_FAILED(sample->SetSampleDuration(333333));

	// generate frame
	wil::com_ptr_nothrow<IMFSample> outSample;
	RETURN_IF_FAILED(_generator.Generate(sample.get(), _format, &outSample));

	if (pToken)
	{
		RETURN_IF_FAILED(outSample->SetUnknown(MFSampleExtension_Token, pToken));
	}
	RETURN_IF_FAILED(_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, outSample.get()));
	return S_OK;
}

// IMFMediaStream2
STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value)
{
	WINTRACE(L"MediaStream::SetStreamState current:%u value:%u", _state, value);
	// Was `if (_state = value)` -- an assignment, not a comparison, so it silently overwrote
	// _state and then (since PAUSED=1 and RUNNING=2 are both truthy) returned S_OK before ever
	// reaching the switch below for those two states. In practice this meant a caller that starts
	// or pauses the stream via SetStreamState (e.g. Start(nullptr)'s own entry point) never
	// actually ran Start()/the PAUSED transition's own guard -- fixed to the evident intent, an
	// idempotency check ("already in the requested state, nothing to do").
	if (_state == value)
		return S_OK;
	switch (value)
	{
	case MF_STREAM_STATE_PAUSED:
		if (_state != MF_STREAM_STATE_RUNNING)
			RETURN_HR(MF_E_INVALID_STATE_TRANSITION);

		_state = value;
		break;

	case MF_STREAM_STATE_RUNNING:
		RETURN_IF_FAILED(Start(nullptr));
		break;

	case MF_STREAM_STATE_STOPPED:
		RETURN_IF_FAILED(Stop());
		break;

	default:
		RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
		break;
	}
	return S_OK;
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* value)
{
	WINTRACE(L"MediaStream::GetStreamState state:%u", _state);
	RETURN_HR_IF_NULL(E_POINTER, value);
	*value = _state;
	return S_OK;
}

// IKsControl
STDMETHODIMP_(NTSTATUS) MediaStream::KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsProperty len:%u data:%p dataLength:%u", length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, property);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsProperty prop:%s", PKSIDENTIFIER_ToString(property, length).c_str());

	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsMethod len:%u data:%p dataLength:%u", length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, method);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsMethod method:%s", PKSIDENTIFIER_ToString(method, length).c_str());

	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MediaStream::KsEvent(PKSEVENT evt, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	WINTRACE(L"MediaStream::KsEvent evt:%p len:%u data:%p dataLength:%u", evt, length, data, dataLength);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	winrt::slim_lock_guard lock(_lock);

	WINTRACE(L"MediaStream::KsEvent event:%s", PKSIDENTIFIER_ToString(evt, length).c_str());
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
