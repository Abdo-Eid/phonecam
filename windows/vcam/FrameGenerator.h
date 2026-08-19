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

	// Phase 7 (vcam real resolution + rotation): no longer takes a size -- just opens the D3D
	// device handle. EnsureRenderTarget is now the single place resources get (re)sized, called
	// once the negotiated MF_MT_FRAME_SIZE is actually known (see MediaStream::Start).
	HRESULT SetD3DManager(IUnknown* manager);
	// True once SetD3DManager has been called successfully -- distinct from "resources are
	// sized to the current width/height", which EnsureRenderTarget tracks itself (via _width/
	// _height) so a negotiated-size change is never misread as "not GPU mode, reinit from
	// scratch".
	const bool IsGpuMode() const;
	// Idempotent: no-ops if already sized to width x height (checked via _width/_height), else
	// tears down and recreates every GPU/D2D resource -- including the NV12 converter, which is
	// always fully recreated rather than re-typed, since re-typing a live MFT needs its own
	// flush/renegotiation dance that isn't worth it for an operation that only happens on an
	// actual resolution change.
	HRESULT EnsureRenderTarget(UINT width, UINT height);
	HRESULT Generate(IMFSample* sample, REFGUID format, IMFSample** outSample);
};