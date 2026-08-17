#pragma once

#include <memory>

#include "shm/SharedFrameRing.h"

class FrameGenerator
{
	UINT _width;
	UINT _height;
	ULONGLONG _frame;
	MFTIME _prevTime;
	UINT _fps;
	HANDLE _deviceHandle;
	wil::com_ptr_nothrow<ID3D11Texture2D> _texture;
	wil::com_ptr_nothrow<ID2D1RenderTarget> _renderTarget;
	wil::com_ptr_nothrow<ID2D1SolidColorBrush> _whiteBrush;
	wil::com_ptr_nothrow<IDWriteTextFormat> _textFormat;
	wil::com_ptr_nothrow<IDWriteFactory> _dwrite;
	wil::com_ptr_nothrow<IMFTransform> _converter;
	wil::com_ptr_nothrow<IWICBitmap> _bitmap;
	wil::com_ptr_nothrow<IMFDXGIDeviceManager> _dxgiManager;

	// Phase 1b: frames come from phonecam-host via shared memory instead of
	// being drawn here. _ring is opened lazily (the host may not have
	// created it yet) and _nv12Scratch/_rgb32Scratch are reused across calls
	// to avoid a per-frame heap allocation.
	phonecam::shm::SharedFrameRing _ring;
	bool _ringOpenAttempted;
	std::unique_ptr<BYTE[]> _nv12Scratch;
	std::unique_ptr<BYTE[]> _rgb32Scratch;

	HRESULT CreateRenderTargetResources(UINT width, UINT height);
	HRESULT DrawLatestRingFrameOrWaitingMessage();

public:
	FrameGenerator() :
		_width(0),
		_height(0),
		_frame(0),
		_fps(0),
		_deviceHandle(nullptr),
		_prevTime(MFGetSystemTime()),
		_ringOpenAttempted(false)
	{

	}

	~FrameGenerator()
	{
		if (_dxgiManager && _deviceHandle)
		{
			auto hr = _dxgiManager->CloseDeviceHandle(_deviceHandle); // don't report error at that point
			if (FAILED(hr))
			{
				WINTRACE(L"FrameGenerator CloseDeviceHandle: 0x%08X", hr);
			}
		}
	}

	HRESULT SetD3DManager(IUnknown* manager, UINT width, UINT height);
	const bool HasD3DManager() const;
	HRESULT EnsureRenderTarget(UINT width, UINT height);
	HRESULT Generate(IMFSample* sample, REFGUID format, IMFSample** outSample);
};