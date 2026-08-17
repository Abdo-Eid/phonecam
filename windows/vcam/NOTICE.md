# Third-party notice

The COM media source in this directory (`MediaSource.cpp`/`.h`, `MediaStream.cpp`/`.h`,
`Activator.cpp`/`.h`, `EnumNames.*`, `MFTools.*`, `Tools.*`, `WinTrace.*`, `FrameGenerator.*`,
`dllmain.cpp`, `pch.*`, `framework.h`, `Undocumented.h`, `resource.h`, and the
`.def`/`.rc`/`.vcxproj*`/`packages.config` project files) is forked from
[VCamSample](https://github.com/smourier/VCamSample) by Simon Mourier, used
under the MIT License:

```
MIT License

Copyright (c) 2024-2026 Simon Mourier

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Modifications (renaming, a new CLSID, and — starting with the `SharedFrameRing`
integration — replacing the built-in `FrameGenerator` test pattern with frames
received from an Android phone over USB) are Copyright (C) 2026 PhoneCam
Project, licensed under this repository's [GPL-3.0-or-later](../../LICENSE),
consistent with the MIT license's one-way compatibility with GPL.

An unmodified copy of the original project is kept at
[`third_party/reference/VCamSample`](../../third_party/reference/VCamSample)
for comparison.
