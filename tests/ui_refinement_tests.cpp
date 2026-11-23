// Native rendering and layout fixtures remain private and never shown.
#define wWinMain CleanTaskManEntryForTests
#include "../src/main.cpp"
#undef wWinMain
#include <cstdio>
#include <wincodec.h>
// Read only this private fixture's Direct2D surface. No desktop capture,
// visible window, user-app input, or production rendering changes are used.
bool exportFixture(App& app, const wchar_t* name) {
    auto originalTarget = app.target.p;
    app.target.p = nullptr;
    auto originalBrush = app.brush.p;
    app.brush.p = nullptr;
    RECT bounds{};
    GetClientRect(app.canvas, &bounds);
    UINT w = bounds.right, h = bounds.bottom;
    auto properties =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                     96 * app.scale, 96 * app.scale, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
    HRESULT hr = app.factory->CreateHwndRenderTarget(
        properties,
        D2D1::HwndRenderTargetProperties(app.canvas, D2D1::SizeU(w, h), D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS),
        app.target.put());
    if (SUCCEEDED(hr))
        hr = app.target->CreateSolidColorBrush(color(app.palette.text), app.brush.put());
    HBITMAP bitmap = nullptr;
    HDC memory = nullptr;
    HGDIOBJ old = nullptr;
    if (SUCCEEDED(hr)) {
        app.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        app.hits.clear();
        app.target->BeginDraw();
        app.target->Clear(color(app.palette.window));
        app.drawTitle();
        app.drawNav();
        app.drawCommands();
        if (app.page == 1)
            app.drawPerformance();
        else
            app.drawSettings();
        Com<ID2D1GdiInteropRenderTarget> interop;
        hr = app.target->QueryInterface(__uuidof(ID2D1GdiInteropRenderTarget),
                                        reinterpret_cast<void**>(interop.put()));
        HDC source = nullptr;
        if (SUCCEEDED(hr))
            hr = interop->GetDC(D2D1_DC_INITIALIZE_MODE_COPY, &source);
        if (SUCCEEDED(hr)) {
            memory = CreateCompatibleDC(source);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = w;
            info.bmiHeader.biHeight = -static_cast<LONG>(h);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            void* pixels = nullptr;
            bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (bitmap) {
                old = SelectObject(memory, bitmap);
                if (!BitBlt(memory, 0, 0, w, h, source, 0, 0, SRCCOPY))
                    hr = E_FAIL;
            } else
                hr = E_FAIL;
            interop->ReleaseDC(nullptr);
        }
        HRESULT ended = app.target->EndDraw();
        if (FAILED(ended))
            hr = ended;
    }
    if (SUCCEEDED(hr) && bitmap) {
        Com<IWICImagingFactory> wic;
        Com<IWICBitmap> source;
        Com<IWICStream> stream;
        Com<IWICBitmapEncoder> encoder;
        Com<IWICBitmapFrameEncode> frame;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.put()));
        auto path = settings::executableFolder() + L"\\" + name;
        if (SUCCEEDED(hr))
            hr = wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, source.put());
        if (SUCCEEDED(hr))
            hr = wic->CreateStream(stream.put());
        if (SUCCEEDED(hr))
            hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr))
            hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
        if (SUCCEEDED(hr))
            hr = encoder->Initialize(stream.p, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr))
            hr = encoder->CreateNewFrame(frame.put(), nullptr);
        if (SUCCEEDED(hr))
            hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr))
            hr = frame->SetSize(w, h);
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGR;
        if (SUCCEEDED(hr))
            hr = frame->SetPixelFormat(&pixelFormat);
        if (SUCCEEDED(hr))
            hr = frame->WriteSource(source.p, nullptr);
        if (SUCCEEDED(hr))
            hr = frame->Commit();
        if (SUCCEEDED(hr))
            hr = encoder->Commit();
    }
    if (old)
        SelectObject(memory, old);
    if (bitmap)
        DeleteObject(bitmap);
    if (memory)
        DeleteDC(memory);
    app.brush.reset();
    app.target.reset();
    app.target.p = originalTarget;
    app.brush.p = originalBrush;
    if (FAILED(hr))
        printf("EXPORT_ERROR %08lx\n", static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok)
            failures++;
    };
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto name = L"CleanTaskMan.HiddenRenderFixture." + std::to_wstring(GetCurrentProcessId());
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = App::proc;
    wc.lpszClassName = name.c_str();
    RegisterClassW(&wc);
    {
        App app;
        app.page = 1;
        app.metricsPaused = true;
        HWND fixture =
            CreateWindowExW(0, name.c_str(), L"Never-shown rendering fixture", WS_POPUP | WS_CLIPCHILDREN, 0,
                            0, 1180, 800, nullptr, nullptr, wc.hInstance, &app);
        check(fixture && !IsWindowVisible(fixture), "native rendering fixture stays hidden");
        if (!fixture) {
            printf("CREATE_ERROR %lu\n", GetLastError());
            return 1;
        }
        auto devices = std::make_shared<telemetry::DeviceSnapshot>();
        devices->ready = true;
        telemetry::Device disk;
        disk.kind = telemetry::Kind::Disk;
        disk.key = L"disk:0";
        disk.name = L"Disk 0 (C:)";
        disk.type = L"SSD (NVMe)";
        disk.color = 0x87C879;
        devices->devices.push_back(disk);
        telemetry::Device gpu;
        gpu.kind = telemetry::Kind::Gpu;
        gpu.key = L"gpu:fixture";
        gpu.name = L"GPU 0";
        gpu.description = L"NVIDIA GeForce RTX 5070";
        gpu.color = 0x97A8F4;
        devices->devices.push_back(gpu);
        app.devices = devices;
        InvalidateRect(fixture, nullptr, FALSE);
        app.draw();
        check(app.target && app.target->GetPixelFormat().alphaMode == D2D1_ALPHA_MODE_IGNORE,
              "render target ignores alpha for an opaque client surface");
        check(app.canvas && app.target->GetHwnd() == app.canvas && GetParent(app.canvas) == fixture &&
                  GetParent(app.search) == fixture && !IsChild(app.canvas, app.search),
              "Direct2D renderer and native EDIT use separate sibling HWND surfaces");
        COLORREF keyColor = 0;
        BYTE alpha = 0;
        DWORD attributes = 0;
        check(GetLayeredWindowAttributes(app.search, &keyColor, &alpha, &attributes) && alpha == 255 &&
                  attributes == LWA_ALPHA &&
                  !(GetWindowLongPtrW(app.search, GWL_EXSTYLE) & WS_EX_TRANSPARENT),
              "native EDIT has constant full opacity with no color key or click-through style");
        check(!(GetClassLongPtrW(app.search, GCL_STYLE) & (CS_OWNDC | CS_CLASSDC)) &&
                  IsWindowEnabled(app.search),
              "native editor class supports layering and remains enabled");
        SetWindowTextW(app.search, L"native editing");
        SendMessageW(app.search, EM_SETSEL, 0, 6);
        DWORD selectionStart = 0, selectionEnd = 0;
        SendMessageW(app.search, EM_GETSEL, reinterpret_cast<WPARAM>(&selectionStart),
                     reinterpret_cast<LPARAM>(&selectionEnd));
        wchar_t nativeClass[32]{};
        GetClassNameW(app.search, nativeClass, 32);
        check(wcscmp(nativeClass, L"Edit") == 0 && selectionStart == 0 && selectionEnd == 6 &&
                  GetWindowTextLengthW(app.search) == 14,
              "opaque surface retains the real native EDIT text and selection model");
        SetWindowTextW(app.search, L"");
        RECT rootBounds{}, canvasBounds{};
        GetClientRect(fixture, &rootBounds);
        GetClientRect(app.canvas, &canvasBounds);
        check(EqualRect(&rootBounds, &canvasBounds) && GetWindow(app.search, GW_HWNDNEXT) == app.canvas &&
                  (GetWindowLongPtrW(app.search, GWL_STYLE) & WS_CLIPSIBLINGS) &&
                  (GetWindowLongPtrW(app.canvas, GWL_STYLE) & WS_CLIPSIBLINGS),
              "canvas matches root pixels and stays below the clipped native EDIT sibling");
        check(App::canvasProc(app.canvas, WM_NCHITTEST, 0, 0) == HTTRANSPARENT,
              "rendering surface leaves custom-frame input routed to its parent");
        bool rows = true;
        float y = -1;
        unsigned count = 0;
        for (auto& h : app.hits)
            if (h.id >= 200 && h.id < 300) {
                rows = rows && h.box.h == 64 && (y < 0 || h.box.y - y == 64);
                y = h.box.y;
                count++;
            }
        check(rows && count == 4,
              "all Performance rows share 64-DIP height with equal two-DIP top/bottom padding");
        telemetry::Device network;
        network.kind = telemetry::Kind::Network;
        network.type = L"Ethernet";
        network.name = L"Fiber Optic";
        check(App::performanceTitle(network) == L"Ethernet" && network.name == L"Fiber Optic",
              "network heading uses connection type and preserves adapter alias for its second line");
        auto initialFont = app.editFont;
        auto initialGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 24; i++) {
            SetWindowPos(fixture, nullptr, 0, 0, 1000 + i * 7, 800,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(fixture, nullptr, FALSE);
            app.draw();
        }
        check(app.editFont == initialFont &&
                  GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= initialGdi + 2,
              "resize reuses the edit font and does not accumulate GDI resources");
        app.pointerKnown = true;
        app.pointerX = app.navWidth() + 100;
        app.pointerY = 108 + App::performanceRowHeight + 30;
        app.hover = 201;
        app.acceptSnapshots();
        check(app.hover == 201, "sensor refresh preserves stationary hover on a nonselected Performance row");
        app.pointerY = 108 + 2 * App::performanceRowHeight + 30;
        app.synchronizeHover();
        check(app.hover == 202, "hover tracks rebuilt hit rectangles");
        HDC memory = CreateCompatibleDC(nullptr);
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = 1400;
        bitmap.bmiHeader.biHeight = -900;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP surface = CreateDIBSection(memory, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
        auto old = SelectObject(memory, surface);
        app.eraseOpaque(memory);
        check(GetPixel(memory, 2, 2) == App::gdiColor(app.palette.window) &&
                  GetPixel(memory, 1000, 700) == App::gdiColor(app.palette.window),
              "exposed resize background is filled with the interface color");
        SelectObject(memory, old);
        DeleteObject(surface);
        DeleteDC(memory);
        Com<IDWriteTextLayout> layout;
        auto* format = app.font(11);
        HRESULT hr =
            app.writer->CreateTextLayout(gpu.description.c_str(), static_cast<UINT32>(gpu.description.size()),
                                         format, 142, 16, layout.put());
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(hr))
            hr = layout->GetMetrics(&metrics);
        check(SUCCEEDED(hr) && metrics.height <= 16,
              "11-DIP metadata fits the compact third-line baseline height");
        check(app.secondarySize(L"Fiber Optic", 142) == 12 &&
                  app.secondarySize(std::wstring(100, L'W'), 142) == 11,
              "sidebar metadata grows one DIP only when its measured width fits");
        auto* regular = app.font(24, 400);
        check(regular->GetFontWeight() == DWRITE_FONT_WEIGHT_NORMAL,
              "bottom output values retain regular weight at the reduced size");
        gpu.usage = 13;
        gpu.dedicatedUsed = 2.3 * 1073741824;
        gpu.capacity = 12ull * 1073741824;
        gpu.sharedUsed = 135.9 * 1048576;
        gpu.sharedCapacity = 48ull * 1073741824;
        gpu.temperature = 32;
        gpu.driverVersion = L"32.0.16.2114";
        gpu.driverDate = L"2026-10-27";
        gpu.physicalLocation = L"PCIe 2, Dev 0, Fun 0";
        for (unsigned n = 0; n <= 60; n++) {
            double t = 1000 + n;
            gpu.history.push_back({t, .13});
            gpu.dedicatedHistory.push_back({t, gpu.dedicatedUsed * (.9 + .1 * n / 60)});
            gpu.sharedHistory.push_back({t, gpu.sharedUsed});
            for (size_t engine = 0; engine < 4; engine++) {
                auto& series = gpu.gpuEngines[engine];
                series.usage = engine == 0 ? 13 : engine == 1 ? 2 : 0;
                series.history.push_back({t, engine == 0 ? .05 + .08 * (n % 9) / 8 : engine == 1 ? .02 : 0});
            }
        }
        devices->devices.back() = gpu;
        app.devices = devices;
        app.device = 3;
        app.scroll = 0;
        app.hover = -1;
        auto sideGpu = app.gpuReadouts(gpu, 440);
        check(sideGpu.alongside && sideGpu.detailsY < 2 &&
                  sideGpu.detailsX + sideGpu.labelWidth + 8 + sideGpu.valueWidth <= 440,
              "GPU metadata remains beside readable values at the compact-navigation minimum width");
        bool stableGpuOrder = true;
        for (float available : {288.0f, 440.0f, 480.0f, 600.0f, 840.0f, 1600.0f}) {
            auto reading = app.gpuReadouts(gpu, available);
            stableGpuOrder = stableGpuOrder && reading.fields[0].y == reading.fields[3].y &&
                             reading.fields[3].x > reading.fields[0].x &&
                             reading.fields[1].y > reading.fields[0].y &&
                             reading.fields[2].y > reading.fields[1].y;
        }
        check(stableGpuOrder, "every GPU width keeps Utilization beside Temperature, then Dedicated memory, "
                              "then Shared memory");
        // Render deterministic layout data only in this QA fixture.
        SetWindowPos(fixture, nullptr, 0, 0, static_cast<int>(1180 * app.scale),
                     static_cast<int>(780 * app.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        check(exportFixture(app, L"gpu-layout-wide.png"),
              "private native GPU rendering exports wide layout without desktop access");
        check(app.contentHeight <= app.viewHeight,
              "GPU graphs and details fit the default window without increasing its size");
        SetWindowPos(fixture, nullptr, 0, 0, static_cast<int>(800 * app.scale),
                     static_cast<int>(540 * app.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        check(exportFixture(app, L"gpu-layout-small.png") && app.contentHeight > app.viewHeight,
              "small GPU layout scrolls rather than clipping lower metadata");
        app.scroll = app.contentHeight - app.viewHeight;
        check(exportFixture(app, L"gpu-layout-small-bottom.png"),
              "small-window GPU details remain reachable by scrolling");
        SetWindowPos(fixture, nullptr, 0, 0, static_cast<int>(760 * app.scale),
                     static_cast<int>(716 * app.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        app.scroll = 0;
        check(exportFixture(app, L"gpu-layout-narrow-tall.png") && app.contentHeight <= app.viewHeight,
              "narrow tall GPU view keeps its metadata beside readings and fits without vertical scrolling");
        bool gpuNamesFit = true;
        for (const wchar_t* model : {L"NVIDIA GeForce RTX 5070", L"NVIDIA GeForce RTX 5080"}) {
            Com<IDWriteTextLayout> nameLayout;
            DWRITE_LINE_METRICS nameLine{};
            UINT32 lineCount = 0;
            float nameWidth = app.performanceRailWidth() - 92;
            auto result = app.writer->CreateTextLayout(model, static_cast<UINT32>(wcslen(model)),
                                                       app.font(app.secondarySize(model, nameWidth)),
                                                       nameWidth, 17, nameLayout.put());
            if (SUCCEEDED(result))
                result = nameLayout->GetLineMetrics(&nameLine, 1, &lineCount);
            gpuNamesFit = gpuNamesFit && SUCCEEDED(result) && lineCount == 1 && !nameLine.isTrimmed;
        }
        check(gpuNamesFit,
              "full RTX 5070 and RTX 5080 names render without ellipsis at minimum window width");
        float minimumPanel = app.width - app.navWidth() - app.performanceRailWidth() - 62;
        auto minimumGpu = app.gpuReadouts(gpu, minimumPanel);
        check(minimumGpu.alongside &&
                  minimumGpu.detailsX + minimumGpu.labelWidth + 8 + minimumGpu.valueWidth <= minimumPanel,
              "wider minimum device rail preserves adjacent GPU metadata within the right panel");
        for (bool expanded : {false, true}) {
            app.navExpanded = expanded;
            app.layout();
            bool stationary = true;
            float firstRight = -1;
            const float oldBreakpoint = app.navWidth() + 760;
            for (float resizedWidth :
                 {760.0f, oldBreakpoint - 1, oldBreakpoint - .5f, oldBreakpoint, oldBreakpoint + .5f,
                  oldBreakpoint + 1, 1180.0f, 1600.0f, 1180.0f, oldBreakpoint, oldBreakpoint - 1, 760.0f}) {
                SetWindowPos(fixture, nullptr, 0, 0, static_cast<int>(resizedWidth * app.scale),
                             static_cast<int>(716 * app.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                app.draw();
                auto row =
                    std::find_if(app.hits.begin(), app.hits.end(), [](const auto& h) { return h.id == 200; });
                if (row == app.hits.end()) {
                    stationary = false;
                    continue;
                }
                float edge = row->box.x + row->box.w;
                if (firstRight < 0)
                    firstRight = edge;
                else
                    stationary = stationary && std::abs(edge - firstRight) < .01f;
            }
            check(stationary, expanded ? "expanded-navigation device column stays stationary across the old "
                                         "resize breakpoint in both directions"
                                       : "compact-navigation device column stays stationary across the old "
                                         "resize breakpoint in both directions");
        }
        app.navExpanded = false;
        app.layout();
        app.scroll = 0;
        app.hover = -1;
        app.tooltip.clear();
        SetWindowPos(fixture, nullptr, 0, 0, static_cast<int>(800 * app.scale),
                     static_cast<int>(540 * app.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        app.navExpanded = true;
        app.layout();
        app.scroll = 10000;
        check(exportFixture(app, L"gpu-layout-expanded-nav.png") && app.contentHeight > app.viewHeight,
              "expanded navigation keeps GPU readings and metadata reachable in a narrow content panel");
        app.navExpanded = false;
        app.layout();
        app.scroll = 0;
        app.themeMode = 2;
        app.updateTheme();
        check(exportFixture(app, L"gpu-layout-light.png"),
              "GPU graph and metadata render in the existing light palette");
        app.themeMode = 1;
        app.updateTheme();
        disk.description = L"WD_BLACK SN850X 4000GB";
        disk.capacity = 4000000000000ull;
        disk.storage.formattedCapacity = 3999000000000.0;
        disk.storage.systemDisk = false;
        disk.storage.pageFile = false;
        disk.usage = 2;
        disk.read = 1.2 * 1048576;
        disk.write = .7 * 1048576;
        disk.responseMs = 1.2;
        for (int n = 0; n <= 60; n++)
            disk.history.push_back({1000.0 + n, .01 + .01 * (n % 5) / 4});
        devices->devices.front() = disk;
        app.device = 2;
        app.scroll = 0;
        auto sideDisk = app.diskReadouts(disk, 440);
        check(sideDisk.alongside && sideDisk.detailsY < 2 &&
                  sideDisk.detailsX + sideDisk.labelWidth + 8 + sideDisk.valueWidth <= 440,
              "Storage metadata remains beside both reading columns at the compact-navigation minimum width");
        SetWindowPos(fixture, nullptr, 0, 0, static_cast<int>(1180 * app.scale),
                     static_cast<int>(780 * app.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        check(exportFixture(app, L"storage-layout-wide.png"),
              "storage panel renders all five metadata rows to the right of its readings");
        SetWindowPos(fixture, nullptr, 0, 0, static_cast<int>(800 * app.scale),
                     static_cast<int>(540 * app.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        check(exportFixture(app, L"storage-layout-small.png"),
              "small storage panel keeps its lower metadata within the window");
        SetWindowPos(fixture, nullptr, 0, 0, static_cast<int>(760 * app.scale),
                     static_cast<int>(540 * app.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        check(exportFixture(app, L"storage-layout-minimum.png"),
              "minimum-width Storage renders compact readings beside the metadata column");
        auto minimumDisk =
            app.diskReadouts(disk, app.width - app.navWidth() - app.performanceRailWidth() - 62);
        check(minimumDisk.alongside,
              "wider minimum device rail preserves side-by-side Storage readings and metadata");
        auto memoryCore = std::make_shared<telemetry::CoreSnapshot>();
        memoryCore->memory.valid = true;
        memoryCore->memory.total = 96ull * 1073741824;
        memoryCore->memory.available = 72ull * 1073741824;
        for (int n = 0; n <= 60; n++)
            memoryCore->memoryHistory.push_back({1000.0 + n, .25});
        app.core = memoryCore;
        app.device = 1;
        SetWindowPos(fixture, nullptr, 0, 0, static_cast<int>(1180 * app.scale),
                     static_cast<int>(780 * app.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        check(exportFixture(app, L"memory-layout-wide.png"),
              "Memory renders its denser grid with regular reduced-size outputs");
        memoryCore->ready = true;
        memoryCore->cpu = 7;
        memoryCore->memory.performanceValid = true;
        memoryCore->info.name = L"Intel(R) Core(TM) i9-14900K";
        memoryCore->info.logical = 32;
        memoryCore->info.cores = 24;
        memoryCore->info.sockets = 1;
        memoryCore->info.baseGHz = 3.19;
        memoryCore->totalProcesses = 158;
        memoryCore->totalThreads = 4124;
        memoryCore->totalHandles = 81318;
        memoryCore->upMilliseconds = 12152000;
        for (int n = 0; n <= 60; n++)
            memoryCore->cpuHistory.push_back({1000.0 + n, .07});
        devices->frequencyGHz = 4.23;
        app.device = 0;
        check(exportFixture(app, L"cpu-layout-wide.png"),
              "CPU renders the smaller primary values alongside unchanged counts, uptime and metadata");
        network.key = L"net:fixture";
        network.description = L"Intel(R) Ethernet Connection";
        network.color = 0xD9AB75;
        network.write = 250;
        network.read = 6000;
        network.ipv4 = L"192.0.2.8";
        for (int n = 0; n <= 60; n++)
            network.history.push_back({1000.0 + n, 50000});
        devices->devices.push_back(network);
        app.device = 4;
        check(exportFixture(app, L"network-layout-wide.png"),
              "Ethernet renders the smaller Send and Receive values alongside unchanged metadata");
        app.page = 7;
        app.scroll = 0;
        SetWindowPos(fixture, nullptr, 0, 0, static_cast<int>(1180 * app.scale),
                     static_cast<int>(780 * app.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        check(exportFixture(app, L"settings-layout.png"),
              "settings layout exports without a manual Save command");
        app.scroll = 10000;
        check(exportFixture(app, L"settings-layout-bottom.png"),
              "automatic-save configuration details remain visible at the bottom of Settings");
        check(std::none_of(app.hits.begin(), app.hits.end(),
                           [](const auto& h) { return h.id == 306 || h.id == 670 || h.id == 641; }),
              "Settings has no manual Save button or obsolete automatic-placement toggle");
        app.page = 1;
        app.device = 0;
        app.scroll = 0;
        check(!app.collectorsStarted && app.collector->startedAt() == 0,
              "hidden rendering and resizing never start sensors");
        // Isolate this thread's modifier snapshot from a user holding Ctrl
        // while the native fixture runs. This emits no keyboard input.
        BYTE originalKeys[256]{}, fixtureKeys[256]{};
        GetKeyboardState(originalKeys);
        memcpy(fixtureKeys, originalKeys, sizeof(fixtureKeys));
        fixtureKeys[VK_CONTROL] = fixtureKeys[VK_LCONTROL] = fixtureKeys[VK_RCONTROL] = 0;
        SetKeyboardState(fixtureKeys);
        App::editProc(app.search, WM_KEYDOWN, VK_TAB, 0, 1, reinterpret_cast<DWORD_PTR>(&app));
        SetKeyboardState(originalKeys);
        check(app.focusArea == 3, "Tab from Performance search enters device-list keyboard navigation");
        check(!IsWindowVisible(fixture), "all fixture checks finish without showing a window");
        auto savedBefore = settings::load(settings::filePath());
        auto closedAt = telemetry::clockSeconds();
        App::proc(fixture, WM_CLOSE, 0, 0);
        double closeMs = (telemetry::clockSeconds() - closedAt) * 1000;
        printf("HIDDEN_CLOSE handler_ms=%.3f\n", closeMs);
        check(!IsWindow(fixture) && !app.placementQueued &&
                  settings::load(settings::filePath()) == savedBefore,
              "true close destroys its private window without saving preferences or unchanged placement");
        app.window = nullptr;
    }
    {
        App app;
        app.metricsPaused = true;
        app.persistedPreferences["Configuration.Revision"] = "session-fixture";
        HWND fixture =
            CreateWindowExW(0, name.c_str(), L"Never-shown session fixture", WS_POPUP | WS_CLIPCHILDREN, 0, 0,
                            1000, 700, nullptr, nullptr, wc.hInstance, &app);
        app.placementBaseline = app.currentPlacement();
        SetWindowPos(fixture, nullptr, 50, 60, 1000, 700, SWP_NOZORDER | SWP_NOACTIVATE);
        DeleteFileW(settings::placementPath().c_str());
        auto mainBefore = settings::load(settings::filePath());
        App::proc(fixture, WM_ENDSESSION, FALSE, 0);
        check(IsWindow(fixture) &&
                  GetFileAttributesW(settings::placementPath().c_str()) == INVALID_FILE_ATTRIBUTES,
              "cancelled session end performs no placement write");
        App::proc(fixture, WM_ENDSESSION, TRUE, 0);
        auto saved = settings::load(settings::placementPath());
        check(!IsWindow(fixture) && settings::integer(saved, "Window.Left", 0, 0, 1000) == 50 &&
                  settings::load(settings::filePath()) == mainBefore,
              "confirmed session end drains only changed placement before returning");
        app.window = nullptr;
        DeleteFileW(settings::placementPath().c_str());
    }
    UnregisterClassW(name.c_str(), wc.hInstance);
    CoUninitialize();
    printf("RESULT failures=%d; no user-window input or desktop capture\n", failures);
    return failures ? 1 : 0;
}
