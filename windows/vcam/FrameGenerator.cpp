#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "EnumNames.h"
#include "MFTools.h"
#include "FrameGenerator.h"

HRESULT FrameGenerator::EnsureRenderTarget(UINT width, UINT height)
{
	// Idempotent: called on every MediaStream::Start (including the Start(nullptr) path from
	// SetStreamState), so a repeat call at the same size that's already set up must be a cheap
	// no-op, not a resource-recreation storm.
	if (_renderTarget && _width == width && _height == height)
	{
		_prevTime = MFGetSystemTime();
		_frame = 0;
		return S_OK;
	}

	// A real resize (or first-time init): drop everything sized to the old dimensions before
	// rebuilding. _dxgiManager/_deviceHandle are NOT reset here -- SetD3DManager owns those, and
	// a resolution change on an already-GPU-mode stream must not need a fresh SetD3DManager call.
	_renderTarget.reset();
	_texture.reset();
	_bitmap.reset();
	_whiteBrush.reset();
	_textFormat.reset();
	_converter.reset();

	if (IsGpuMode())
	{
		wil::com_ptr_nothrow<ID3D11Device> device;
		RETURN_IF_FAILED(_dxgiManager->GetVideoService(_deviceHandle, IID_PPV_ARGS(&device)));

		// create a texture/surface to write
		CD3D11_TEXTURE2D_DESC desc
		(
			DXGI_FORMAT_B8G8R8A8_UNORM,
			width,
			height,
			1,
			1,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET
		);
		RETURN_IF_FAILED(device->CreateTexture2D(&desc, nullptr, &_texture));
		wil::com_ptr_nothrow<IDXGISurface> surface;
		RETURN_IF_FAILED(_texture.copy_to(&surface));

		// create a D2D1 render target from 2D GPU surface
		wil::com_ptr_nothrow<ID2D1Factory> d2d1Factory;
		RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, IID_PPV_ARGS(&d2d1Factory)));

		auto props = D2D1::RenderTargetProperties
		(
			D2D1_RENDER_TARGET_TYPE_DEFAULT,
			D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)
		);
		RETURN_IF_FAILED(d2d1Factory->CreateDxgiSurfaceRenderTarget(surface.get(), props, &_renderTarget));

		RETURN_IF_FAILED(CreateRenderTargetResources(width, height));

		// create GPU RGB => NV12 converter, fresh at the new size -- re-typing a live MFT needs
		// its own flush/SetInputType(0,nullptr,0) dance; recreating outright is simpler and this
		// only runs on an actual resolution change, not per-frame.
		RETURN_IF_FAILED(CoCreateInstance(CLSID_VideoProcessorMFT, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&_converter)));

		wil::com_ptr_nothrow<IMFAttributes> atts;
		RETURN_IF_FAILED(_converter->GetAttributes(&atts));
		TraceMFAttributes(atts.get(), L"VideoProcessorMFT");

		MFT_OUTPUT_STREAM_INFO info{};
		RETURN_IF_FAILED(_converter->GetOutputStreamInfo(0, &info));
		WINTRACE(L"FrameGenerator::EnsureRenderTarget CLSID_VideoProcessorMFT flags:0x%08X size:%u alignment:%u width:%u height:%u", info.dwFlags, info.cbSize, info.cbAlignment, width, height);

		wil::com_ptr_nothrow<IMFMediaType> inputType;
		RETURN_IF_FAILED(MFCreateMediaType(&inputType));
		inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
		MFSetAttributeSize(inputType.get(), MF_MT_FRAME_SIZE, width, height);
		RETURN_IF_FAILED(_converter->SetInputType(0, inputType.get(), 0));

		wil::com_ptr_nothrow<IMFMediaType> outputType;
		RETURN_IF_FAILED(MFCreateMediaType(&outputType));
		outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
		MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, width, height);
		RETURN_IF_FAILED(_converter->SetOutputType(0, outputType.get(), 0));

		// make sure the video processor works on GPU
		RETURN_IF_FAILED(_converter->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)_dxgiManager.get()));
	}
	else
	{
		// create a D2D1 render target from WIC bitmap
		wil::com_ptr_nothrow<ID2D1Factory> d2d1Factory;
		RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, IID_PPV_ARGS(&d2d1Factory)));

		wil::com_ptr_nothrow<IWICImagingFactory> wicFactory;
		RETURN_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wicFactory)));

		RETURN_IF_FAILED(wicFactory->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &_bitmap));

		D2D1_RENDER_TARGET_PROPERTIES props{};
		props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
		props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
		RETURN_IF_FAILED(d2d1Factory->CreateWicBitmapRenderTarget(_bitmap.get(), props, &_renderTarget));

		RETURN_IF_FAILED(CreateRenderTargetResources(width, height));
	}

	_prevTime = MFGetSystemTime();
	_frame = 0;
	return S_OK;
}

const bool FrameGenerator::IsGpuMode() const
{
	return _dxgiManager != nullptr;
}

HRESULT FrameGenerator::SetD3DManager(IUnknown* manager)
{
	RETURN_HR_IF_NULL(E_POINTER, manager);

	RETURN_IF_FAILED(manager->QueryInterface(&_dxgiManager));
	RETURN_IF_FAILED(_dxgiManager->OpenDeviceHandle(&_deviceHandle));
	return S_OK;
}

// common to CPU & GPU
HRESULT FrameGenerator::CreateRenderTargetResources(UINT width, UINT height)
{
	assert(_renderTarget);
	RETURN_IF_FAILED(_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &_whiteBrush));

	RETURN_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&_dwrite));
	RETURN_IF_FAILED(_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 40, L"", &_textFormat));
	RETURN_IF_FAILED(_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
	RETURN_IF_FAILED(_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
	_width = width;
	_height = height;
	return S_OK;
}

HRESULT FrameGenerator::DrawLatestRingFrameOrWaitingMessage()
{
	// Lazily (re)try opening the ring: the host may not have started yet, or
	// this may be a fresh instance of the media source in a different
	// process (e.g. the Frame Server loading phonecam-vcam.dll separately
	// from any earlier consumer) that hasn't opened it yet. A failed
	// open here just means "no frame yet", not a fatal error.
	if (!_ring.IsOpen())
	{
		_ring.Open();
	}
	if (_ring.IsOpen())
	{
		// Cheap atomic store, once per draw -- keeps the tray's read-only resolution display fresh
		// across host restarts without a separate "just opened" special case.
		_ring.SetNegotiatedSize(_width, _height);
	}

	UINT32 frameW = 0, frameH = 0;
	UINT64 ts = 0;
	bool gotFrame = false;
	if (_ring.IsOpen())
	{
		if (!_nv12Scratch)
		{
			_nv12Scratch.reset(new BYTE[phonecam::shm::kMaxFrameBytes]);
		}
		gotFrame = _ring.TryReadLatestFrame(frameW, frameH, ts, _nv12Scratch.get(), phonecam::shm::kMaxFrameBytes);
	}

	if (gotFrame)
	{
		if (!_rgb32Scratch)
		{
			_rgb32Scratch.reset(new BYTE[(size_t)phonecam::shm::kMaxWidth * phonecam::shm::kMaxHeight * 4]);
		}
		const LONG nv12Stride = (LONG)frameW;
		const LONG rgbStride = (LONG)frameW * 4;
		RETURN_IF_FAILED(NV12ToRGB32(_nv12Scratch.get(), (ULONG)((size_t)frameW * frameH * 3 / 2), nv12Stride,
			frameW, frameH, _rgb32Scratch.get(), (ULONG)((size_t)frameW * frameH * 4), rgbStride));

		wil::com_ptr_nothrow<ID2D1Bitmap> bitmap;
		auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		RETURN_IF_FAILED(_renderTarget->CreateBitmap(D2D1::SizeU(frameW, frameH), _rgb32Scratch.get(), rgbStride, props, &bitmap));

		phonecam::shm::Rotation rotation = phonecam::shm::Rotation::Deg0;
		bool mirror = false;
		_ring.GetTransform(rotation, mirror);
		const bool swapWH = (rotation == phonecam::shm::Rotation::Deg90 || rotation == phonecam::shm::Rotation::Deg270);
		const FLOAT rotatedSrcW = swapWH ? (FLOAT)frameH : (FLOAT)frameW;
		const FLOAT rotatedSrcH = swapWH ? (FLOAT)frameW : (FLOAT)frameH;
		// Fit rotated source into the negotiated canvas -- 1.0 exactly for the headline case (a
		// 1920x1080 sensor frame rotated 90 into a 1080x1920 canvas), pillarboxed/letterboxed
		// otherwise (e.g. rotating into a landscape-only canvas), never distorted.
		const FLOAT scale = ((FLOAT)_width / rotatedSrcW < (FLOAT)_height / rotatedSrcH)
			? (FLOAT)_width / rotatedSrcW
			: (FLOAT)_height / rotatedSrcH;
		const FLOAT angleDegrees = (FLOAT)(static_cast<int>(rotation) * 90);

		// Composed about the origin, in source-pixel space, with the bitmap drawn into a rect
		// centered at the origin below: rotate first, then mirror (so "mirror" always means "flip
		// what the viewer sees", regardless of the chosen rotation), then scale to fit, then
		// translate to the canvas center.
		const auto transform =
			D2D1::Matrix3x2F::Rotation(angleDegrees) *
			D2D1::Matrix3x2F::Scale(mirror ? -scale : scale, scale) *
			D2D1::Matrix3x2F::Translation((FLOAT)_width / 2.0f, (FLOAT)_height / 2.0f);

		_renderTarget->BeginDraw();
		_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 1));
		_renderTarget->SetTransform(transform);
		_renderTarget->DrawBitmap(bitmap.get(), D2D1::RectF(-(FLOAT)frameW / 2.0f, -(FLOAT)frameH / 2.0f, (FLOAT)frameW / 2.0f, (FLOAT)frameH / 2.0f));
		_renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
		return _renderTarget->EndDraw();
	}

	// No frame published yet: a status message instead of an unexplained
	// black/uninitialized surface.
	_renderTarget->BeginDraw();
	_renderTarget->Clear(D2D1::ColorF(D2D1::ColorF::DarkSlateGray));
	if (_textFormat && _dwrite && _whiteBrush)
	{
		const wchar_t text[] = L"PhoneCam\nWaiting for phonecam-host...";
		wil::com_ptr_nothrow<IDWriteTextLayout> layout;
		if (SUCCEEDED(_dwrite->CreateTextLayout(text, (UINT32)wcslen(text), _textFormat.get(), (FLOAT)_width, (FLOAT)_height, &layout)))
		{
			_renderTarget->DrawTextLayout(D2D1::Point2F(0, 0), layout.get(), _whiteBrush.get());
		}
	}
	return _renderTarget->EndDraw();
}

HRESULT FrameGenerator::Generate(IMFSample* sample, REFGUID format, IMFSample** outSample)
{
	RETURN_HR_IF_NULL(E_POINTER, sample);
	RETURN_HR_IF_NULL(E_POINTER, outSample);
	*outSample = nullptr;

	// render something on image common to CPU & GPU
	if (_renderTarget)
	{
		RETURN_IF_FAILED(DrawLatestRingFrameOrWaitingMessage());
	}

	// build a sample using either D3D/DXGI (GPU) or WIC (CPU)
	wil::com_ptr_nothrow<IMFMediaBuffer> mediaBuffer;
	if (IsGpuMode())
	{
		// remove all existing buffers
		RETURN_IF_FAILED(sample->RemoveAllBuffers());

		// create a buffer from this and add to sample
		RETURN_IF_FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), _texture.get(), 0, 0, &mediaBuffer));
		RETURN_IF_FAILED(sample->AddBuffer(mediaBuffer.get()));

		// if we're on GPU & format is not RGB, convert using GPU
		if (format == MFVideoFormat_NV12)
		{
			assert(_converter);
			RETURN_IF_FAILED(_converter->ProcessInput(0, sample, 0));

			// let converter build the sample for us, note it works because we gave it the D3DManager
			MFT_OUTPUT_DATA_BUFFER buffer = {};
			DWORD status = 0;
			RETURN_IF_FAILED(_converter->ProcessOutput(0, 1, &buffer, &status));
			*outSample = buffer.pSample;
		}
		else
		{
			sample->AddRef();
			*outSample = sample;
		}

		_frame++;
		return S_OK;
	}

	RETURN_IF_FAILED(sample->GetBufferByIndex(0, &mediaBuffer));
	wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;
	BYTE* scanline;
	LONG pitch;
	BYTE* start;
	DWORD length;
	RETURN_IF_FAILED(mediaBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D)));
	RETURN_IF_FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &length));

	wil::com_ptr_nothrow<IWICBitmapLock> lock;
	auto hr = _bitmap->Lock(nullptr, WICBitmapLockRead, &lock);
	// now we're using regular COM macros because we want to be sure to unlock (or we could use try/catch)
	if (SUCCEEDED(hr))
	{
		UINT w, h;
		hr = lock->GetSize(&w, &h);
		if (SUCCEEDED(hr))
		{
			UINT wicStride;
			hr = lock->GetStride(&wicStride);
			if (SUCCEEDED(hr))
			{
				UINT wicSize;
				WICInProcPointer wicPointer;
				hr = lock->GetDataPointer(&wicSize, &wicPointer);
				if (SUCCEEDED(hr))
				{
					WINTRACE(L"WIC stride:%u WIC size:%u MF pitch:%u MF length:%u frame:%u format:%s", wicStride, wicSize, pitch, length, _frame, GUID_ToStringW(format).c_str());
					if (format == MFVideoFormat_NV12)
					{
						// note we could use MF's converter too
						hr = RGB32ToNV12(wicPointer, wicSize, wicStride, w, h, scanline, length, pitch);
					}
					else
					{
						hr = (wicSize != length || wicStride != pitch) ? E_FAIL : S_OK;
						if (SUCCEEDED(hr))
						{
							if (assert_true(wicPointer)) // WIC annotation is currently wrong on GetDataPointer wicPointer arg
							{
								CopyMemory(scanline, wicPointer, length);
							}
						}
					}

					if (SUCCEEDED(hr))
					{
						_frame++;
						sample->AddRef();
						*outSample = sample;
					}
				}
			}
		}
		lock.reset();
	}

	buffer2D->Unlock2D();
	return hr;
}
