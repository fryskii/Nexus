// Hardware: CPU topology / caches / instruction sets / per-core clocks, GPU telemetry, memory
// modules, system board & firmware, and an explicit sensor availability matrix.
#include "ui/pages/Common.h"

namespace nx {

namespace {

enum Tab { HCpu, HGpu, HMem, HBoard, HSensors };

class HardwarePage final : public Page {
public:
    explicit HardwarePage(App& a) : Page(a), cores_(Id("hw.cores")), modules_(Id("hw.modules")), sensors_(Id("hw.sensors")) {
        cores_.columns = {Col("cpu", L"Logical processor", 130), Col("core", L"Core", 60, Align::Right), Col("class", L"Class", 70),
                          Col("util", L"Utilization", 100, Align::Right), Col("kernel", L"Kernel", 80, Align::Right), Col("freq", L"Effective clock", 120, Align::Right)};
        modules_.columns = {Col("slot", L"Slot", 140), Col("bank", L"Bank", 110), Col("size", L"Size", 90, Align::Right), Col("type", L"Type", 70),
                            Col("ff", L"Form factor", 90), Col("speed", L"Rated speed", 100, Align::Right), Col("cfg", L"Configured speed", 120, Align::Right),
                            Col("mfr", L"Manufacturer", 130), Col("part", L"Part number", 160), Col("serial", L"Serial", 110), Col("volt", L"Voltage", 80, Align::Right),
                            Col("rank", L"Rank", 50, Align::Right)};
        sensors_.columns = {Col("sensor", L"Sensor", 240), Col("value", L"Current value", 160), Col("status", L"Availability", 160), Col("source", L"Source / reason", 520)};
        for (Table* t : {&cores_, &modules_, &sensors_}) { t->heatMap = false; t->compact = true; t->showIcons = false; }
        tab_ = static_cast<int>(app.Config().GetInt("hardware.tab", 0));
    }
    const wchar_t* Title() const override { return L"Hardware"; }
    wchar_t Glyph() const override { return 0xE7F4; }
    uint32_t Demand() const override { return demand::PerCore | demand::GpuSystem | demand::GpuSensors | demand::Thermal; }
    void OnShow() override {
        app.EnsureSystemInfo();
        app.EnsureGpuInfo();
        app.EnsureDiskInfo(true);
    }
    void SaveState() override { app.Config().SetInt("hardware.tab", tab_); }
    std::wstring AccessibleSummary() const override { return L"Hardware. " + app.Collector().Cpu().brand; }

    void Draw(Ui& ui, const Rect& area) override {
        Rect r = area;
        Rect bar = r.TakeTop(44);
        int t = ui.Tabs(Id("hw.tabs"), {bar.x, bar.y + 4, bar.w, 32}, {L"Processor", L"Graphics", L"Memory", L"System board", L"Sensors"}, tab_);
        if (t >= 0) { tab_ = t; scroll_ = 0; }
        r.y += 6;
        r.h -= 6;
        switch (tab_) {
        case HCpu: DrawCpu(ui, r); break;
        case HGpu: DrawGpu(ui, r); break;
        case HMem: DrawMem(ui, r); break;
        case HBoard: DrawBoard(ui, r); break;
        case HSensors: DrawSensors(ui, r); break;
        }
    }

private:
    // Two-column key/value card. Returns its height.
    float KvCard(Ui& ui, float x, float y, float w, const std::wstring& title, const std::vector<std::pair<std::wstring, std::wstring>>& rows) {
        float h = 48 + rows.size() * 22.f;
        Rect c{x, y, w, h};
        ui.Card(c);
        ui.g.Text(title, {x + 16, y + 10, w - 32, 24}, Font::BodySemi, ui.t.text);
        float yy = y + 40;
        for (auto& [k, v] : rows) {
            ui.KeyValue(x + 16, yy, std::min(190.f, w * 0.42f), w - std::min(190.f, w * 0.42f) - 32, k, v);
            yy += 22;
        }
        return h;
    }

    void DrawCpu(Ui& ui, Rect r) {
        Gfx& g = ui.g;
        const Theme& th = ui.t;
        const CpuStatic& c = app.Collector().Cpu();
        auto snap = app.Snap();
        float colW = (r.w - 16) / 2;
        float off = scroll_;
        g.PushClip(r);
        float y = r.y - scroll_;
        std::vector<std::pair<std::wstring, std::wstring>> id = {
            {L"Model", c.brand},
            {L"Vendor", c.vendor},
            {L"Family / model / stepping", std::to_wstring(c.family) + L" / " + std::to_wstring(c.model) + L" / " + std::to_wstring(c.stepping)},
            {L"Sockets", std::to_wstring(c.sockets)},
            {L"Cores", std::to_wstring(c.cores) + (c.efficiencyCores ? L" (" + std::to_wstring(c.performanceCores) + L" performance + " + std::to_wstring(c.efficiencyCores) + L" efficiency)" : L"")},
            {L"Logical processors", std::to_wstring(c.logical)},
            {L"NUMA nodes", std::to_wstring(c.numaNodes)},
            {L"Base clock", FormatFreqMHz(c.baseMHz)},
            {L"Maximum clock (reported)", FormatFreqMHz(c.maxMHz)},
        };
        std::vector<std::pair<std::wstring, std::wstring>> live;
        if (snap) {
            live = {{L"Utilization", FormatPercent(snap->cpu.total, 1)},
                    {L"Effective clock (average)", FormatFreqMHz(snap->cpu.currentMHz)},
                    {L"Performance limit", snap->cpu.performanceLimitPct < 99.5f ? FormatPercent(snap->cpu.performanceLimitPct) + L" — throttled by power/thermal policy" : L"Not limited"},
                    {L"Interrupt / DPC time", FormatPercent(snap->cpu.interruptPct, 1) + L" / " + FormatPercent(snap->cpu.dpcPct, 1)},
                    {L"Processor queue length", std::to_wstring(static_cast<int>(snap->cpu.queueLength))},
                    {L"Virtualization", std::wstring(c.virtualizationFirmware ? L"Enabled in firmware" : L"Disabled in firmware") +
                                            (c.virtualizationVendor.empty() ? L"" : L" (" + c.virtualizationVendor + L")")},
                    {L"Second-level address translation", c.slat ? L"Supported" : L"Not supported"},
                    {L"Hypervisor present", c.hypervisorPresent ? L"Yes" : L"No"},
                    {L"Nominal voltage (SMBIOS)", app.Static().sysLoaded && !app.Static().sys.smbios.processors.empty() && app.Static().sys.smbios.processors[0].voltage > 0
                                                      ? std::to_wstring(app.Static().sys.smbios.processors[0].voltage).substr(0, 4) + L" V"
                                                      : L"Not reported"}};
        }
        float h1 = KvCard(ui, r.x, y, colW, L"Identification and topology", id);
        float h2 = KvCard(ui, r.x + colW + 16, y, colW, L"Live state", live);
        y += std::max(h1, h2) + 16;
        // Caches
        std::vector<std::pair<std::wstring, std::wstring>> caches;
        for (auto& ci : c.caches) {
            std::wstring kind = ci.type == L'D' ? L"data" : ci.type == L'I' ? L"instruction" : ci.type == L'T' ? L"trace" : L"unified";
            caches.push_back({L"L" + std::to_wstring(ci.level) + L" " + kind,
                              std::to_wstring(ci.count) + L" × " + FormatBytes(ci.size, 0) + L"  (" + std::to_wstring(ci.associativity) + L"-way, " +
                                  std::to_wstring(ci.lineSize) + L"-byte lines)"});
        }
        float h3 = KvCard(ui, r.x, y, colW, L"Cache hierarchy", caches);
        // Instruction sets as wrapped text.
        std::wstring feats;
        for (auto& f : c.features) { if (!feats.empty()) feats += L"  "; feats += f; }
        float fh = g.MeasureParagraph(feats, colW - 32, Font::Small) + 60;
        Rect fc{r.x + colW + 16, y, colW, std::max(fh, 80.f)};
        ui.Card(fc);
        g.Text(L"Instruction set extensions", {fc.x + 16, fc.y + 10, fc.w - 32, 24}, Font::BodySemi, th.text);
        g.Paragraph(feats, {fc.x + 16, fc.y + 40, fc.w - 32, fc.h - 48}, Font::Small, th.textSecondary);
        y += std::max(h3, fc.h) + 16;
        // Per-core table
        g.Text(L"Per-core load and effective clock", {r.x, y, 400, 24}, Font::BodySemi, th.text);
        y += 28;
        if (snap) {
            coreGrid_.rows.clear();
            uint8_t maxClass = c.coreEfficiencyClass.empty() ? 0 : *std::max_element(c.coreEfficiencyClass.begin(), c.coreEfficiencyClass.end());
            for (size_t i = 0; i < snap->cpu.cores.size(); ++i) {
                GridRow row;
                row.key = i + 1;
                std::wstring cls = i < c.coreEfficiencyClass.size() && maxClass > 0 ? (c.coreEfficiencyClass[i] == maxClass ? L"P-core" : L"E-core") : L"";
                row.cells = {L"CPU " + std::to_wstring(i), i < c.logicalToCore.size() ? std::to_wstring(c.logicalToCore[i]) : L"", cls,
                             FormatPercent(snap->cpu.cores[i], 1), i < snap->cpu.coresKernel.size() ? FormatPercent(snap->cpu.coresKernel[i], 1) : L"",
                             i < snap->cpu.coreMHz.size() && snap->cpu.coreMHz[i] > 0 ? FormatFreqMHz(snap->cpu.coreMHz[i]) : L"—"};
                row.nums = {static_cast<double>(i), i < c.logicalToCore.size() ? static_cast<double>(c.logicalToCore[i]) : 0.0, NoNum(), snap->cpu.cores[i],
                            i < snap->cpu.coresKernel.size() ? snap->cpu.coresKernel[i] : 0.0, i < snap->cpu.coreMHz.size() ? snap->cpu.coreMHz[i] : 0.0};
                coreGrid_.rows.push_back(std::move(row));
            }
            if (cores_.sortCol < 0) { cores_.sortCol = 0; cores_.sortDesc = false; }
            coreGrid_.Refresh(cores_);
            cores_.sortChanged = false;
            float th2 = 34 + coreGrid_.RowCount() * cores_.RowHeight() + 4;
            cores_.scrollY = 0;
            cores_.Draw(ui, {r.x, y, std::min(r.w, 620.f), th2}, coreGrid_);
            y += th2 + 8;
            g.Paragraph(L"Effective clock is derived from the '% Processor Performance' counter multiplied by the base clock, which is how Task Manager reports speed. "
                        L"Per-core temperatures and voltages are not available through public Windows APIs; see the Sensors tab.",
                        {r.x, y, std::min(r.w, 760.f), 60}, Font::Small, th.textTertiary);
            y += 60;
        }
        g.PopClip();
        ui.Scroll(Id("hw.cpu.scroll"), r, y + scroll_ - r.y, off);
        scroll_ = off;
    }

    void DrawGpu(Ui& ui, Rect r) {
        Gfx& g = ui.g;
        auto& st = app.Static();
        auto snap = app.Snap();
        if (!st.gpusLoaded) { g.Text(L"Loading…", r, Font::Body, ui.t.textSecondary, Align::Center); return; }
        float off = scroll_;
        g.PushClip(r);
        float y = r.y - scroll_;
        float colW = (r.w - 16) / 2;
        for (size_t i = 0; i < st.gpus.size(); ++i) {
            auto& gs = st.gpus[i];
            const GpuSnap* live = nullptr;
            if (snap) for (auto& x : snap->gpus) if (x.luid == gs.luid) live = &x;
            auto rep = [&](double v, const std::wstring& s) { return v > 0 ? s : std::wstring(L"Not reported by driver"); };
            std::vector<std::pair<std::wstring, std::wstring>> info = {
                {L"Vendor / device ID", gs.vendor + L"  " + FormatHex(gs.vendorId, 4) + L" / " + FormatHex(gs.deviceId, 4)},
                {L"Driver version", gs.driverVersion},
                {L"Driver date", gs.driverDate},
                {L"DirectX / WDDM", gs.directX + (gs.wddm.empty() ? L"" : L" / WDDM " + gs.wddm)},
                {L"Dedicated video memory", FormatBytes(gs.dedicatedVideo)},
                {L"Shared system memory", FormatBytes(gs.sharedSystem)},
                {L"PCIe link (current)", gs.pcieCurrent.empty() ? L"Not a PCIe device or not reported" : gs.pcieCurrent},
                {L"PCIe link (maximum)", gs.pcieMax},
                {L"Location", gs.location},
                {L"Hardware-accelerated GPU scheduling", gs.hagsSupported < 0 ? L"Unknown" : gs.hagsSupported ? (gs.hagsEnabled ? L"On" : L"Off") : L"Not supported"},
            };
            std::vector<std::pair<std::wstring, std::wstring>> sens;
            if (live) {
                sens = {{L"Utilization (busiest engine)", FormatPercent(live->util, 1)},
                        {L"Temperature", rep(live->temperatureC, TempText(app, live->temperatureC))},
                        {L"Hotspot temperature", L"Not exposed by Windows (vendor tools required)"},
                        {L"Core clock", rep(live->coreFreqMHz, FormatFreqMHz(live->coreFreqMHz) + (live->coreMaxFreqMHz > 0 ? L" (max " + FormatFreqMHz(live->coreMaxFreqMHz) + L")" : L""))},
                        {L"Memory clock", rep(live->memoryFreqMHz, FormatFreqMHz(live->memoryFreqMHz))},
                        {L"Core voltage", rep(live->coreVoltage, std::to_wstring(live->coreVoltage).substr(0, 5) + L" V")},
                        {L"Power", rep(live->powerPct, FormatPercent(live->powerPct, 1) + L" of rated board power")},
                        {L"Fan", rep(live->fanRpm, std::to_wstring(static_cast<int>(live->fanRpm)) + L" RPM" + (gs.maxFanRpm > 0 ? L" of " + std::to_wstring(static_cast<int>(gs.maxFanRpm)) : L""))},
                        {L"Dedicated memory used", FormatBytes(live->dedicatedUsed) + L" / " + FormatBytes(live->dedicatedTotal)},
                        {L"Thermal limits", gs.tempMaxC > 0 ? L"warning " + TempText(app, gs.tempWarnC) + L", max " + TempText(app, gs.tempMaxC) : L"Not reported"}};
            }
            float h1 = KvCard(ui, r.x, y, colW, L"GPU " + std::to_wstring(i) + L": " + gs.name, info);
            float h2 = KvCard(ui, r.x + colW + 16, y, colW, L"Telemetry (driver-reported)", sens);
            y += std::max(h1, h2) + 12;
            if (live && !live->engines.empty()) {
                std::wstring e;
                for (auto& en : live->engines) e += en.name + L" " + FormatPercent(en.util) + L"    ";
                g.Paragraph(L"Engines: " + e, {r.x, y, r.w, 44}, Font::Small, ui.t.textSecondary);
                y += 48;
            }
        }
        if (st.gpus.empty()) g.Text(L"No hardware GPU adapters found.", {r.x, y, r.w, 24}, Font::Body, ui.t.textSecondary);
        y += 8;
        g.Paragraph(L"GPU telemetry comes from the WDDM kernel interface (D3DKMT adapter performance data, WDDM 2.4+), the same source Task Manager uses for GPU temperature. "
                    L"Drivers decide which fields they fill; NEXUS shows 'Not reported' rather than estimating. Throttling reasons and hotspot sensors are vendor-specific.",
                    {r.x, y, std::min(r.w, 820.f), 60}, Font::Small, ui.t.textTertiary);
        y += 64;
        g.PopClip();
        ui.Scroll(Id("hw.gpu.scroll"), r, y + scroll_ - r.y, off);
        scroll_ = off;
    }

    void DrawMem(Ui& ui, Rect r) {
        Gfx& g = ui.g;
        auto& st = app.Static();
        if (!st.sysLoaded) { g.Text(L"Loading…", r, Font::Body, ui.t.textSecondary, Align::Center); return; }
        auto snap = app.Snap();
        const MemStatic& m = st.mem;
        float colW = (r.w - 16) / 2;
        std::vector<std::pair<std::wstring, std::wstring>> sum = {
            {L"Installed", FormatBytes(m.installed)},
            {L"Usable by Windows", snap ? FormatBytes(snap->mem.total) : L""},
            {L"Hardware reserved", snap ? FormatBytes(snap->mem.hardwareReserved) : L""},
            {L"Slots used", std::to_wstring(m.slotsUsed) + L" of " + std::to_wstring(m.slotsTotal)},
            {L"Maximum supported (SMBIOS)", st.sys.smbios.maxMemoryCapacityBytes ? FormatBytes(st.sys.smbios.maxMemoryCapacityBytes) : L""},
            {L"Type / form factor", m.type + L" / " + m.formFactor},
            {L"Speed (configured / rated)", (m.configuredSpeedMTs ? std::to_wstring(m.configuredSpeedMTs) + L" MT/s" : L"?") + L" / " +
                                                (m.speedMTs ? std::to_wstring(m.speedMTs) + L" MT/s" : L"?")},
        };
        std::vector<std::pair<std::wstring, std::wstring>> pressure;
        if (snap) {
            auto& mm = snap->mem;
            pressure = {{L"In use", FormatBytes(mm.InUse()) + L" (" + FormatPercent(mm.total ? 100.0 * mm.InUse() / mm.total : 0) + L")"},
                        {L"Commit charge", FormatBytes(mm.commit) + L" of " + FormatBytes(mm.commitLimit) + L" (" + FormatPercent(mm.commitLimit ? 100.0 * mm.commit / mm.commitLimit : 0) + L")"},
                        {L"Standby / modified / free", FormatBytes(mm.standby) + L" / " + FormatBytes(mm.modified) + L" / " + FormatBytes(mm.free)},
                        {L"Compressed store", FormatBytes(mm.compressed)},
                        {L"Page faults/s (all)", FormatCount(static_cast<uint64_t>(mm.pageFaultsPerSec))},
                        {L"Hard faults (pages read/written)/s", FormatCount(static_cast<uint64_t>(mm.pagesPerSec))},
                        {L"Page file usage", FormatPercent(mm.pagefileUsagePct, 1)}};
        }
        float h1 = KvCard(ui, r.x, r.y, colW, L"Memory configuration", sum);
        float h2 = KvCard(ui, r.x + colW + 16, r.y, colW, L"Memory pressure", pressure);
        float y = r.y + std::max(h1, h2) + 16;
        g.Text(L"Installed modules (SMBIOS type 17)", {r.x, y, 400, 24}, Font::BodySemi, ui.t.text);
        y += 28;
        modGrid_.rows.clear();
        uint64_t k = 1;
        for (auto& d : m.modules) {
            GridRow row;
            row.key = k++;
            bool empty = d.sizeBytes == 0;
            wchar_t volt[32] = L"";
            if (d.configuredVoltageMv) swprintf(volt, 32, L"%.2f V", d.configuredVoltageMv / 1000.0);
            row.cells = {d.deviceLocator, d.bankLocator, empty ? L"Empty" : FormatBytes(d.sizeBytes, 0), empty ? L"" : d.MemoryTypeName(), empty ? L"" : d.FormFactorName(),
                         d.speedMTs ? std::to_wstring(d.speedMTs) + L" MT/s" : L"", d.configuredSpeedMTs ? std::to_wstring(d.configuredSpeedMTs) + L" MT/s" : L"",
                         d.manufacturer, d.partNumber, d.serial, volt, d.rank ? std::to_wstring(d.rank) : L""};
            row.highlight = empty ? 3 : 0;
            modGrid_.rows.push_back(std::move(row));
        }
        modGrid_.Refresh(modules_);
        float th = std::min(r.B() - y - 70, 34 + std::max<size_t>(1, modGrid_.RowCount()) * modules_.RowHeight() + 4);
        modules_.Draw(ui, {r.x, y, r.w, th}, modGrid_);
        y += th + 10;
        g.Paragraph(L"Module details come from the firmware's SMBIOS tables. Memory timings (CL-tRCD-tRP) are stored in each module's SPD chip, which can only be read "
                    L"through the SMBus with a kernel driver; NEXUS does not install drivers, so timings are not shown.",
                    {r.x, y, std::min(r.w, 820.f), 60}, Font::Small, ui.t.textTertiary);
    }

    void DrawBoard(Ui& ui, Rect r) {
        auto& st = app.Static();
        if (!st.sysLoaded) { ui.g.Text(L"Loading…", r, Font::Body, ui.t.textSecondary, Align::Center); return; }
        auto& s = st.sys.smbios;
        float colW = (r.w - 16) / 2;
        std::vector<std::pair<std::wstring, std::wstring>> sys = {{L"Manufacturer", s.systemManufacturer}, {L"Product", s.systemProduct}, {L"Version", s.systemVersion},
                                                                  {L"Family", s.systemFamily}, {L"SKU", s.systemSku}, {L"Serial number", s.systemSerial}, {L"UUID", s.systemUuid}};
        std::vector<std::pair<std::wstring, std::wstring>> board = {{L"Manufacturer", s.boardManufacturer}, {L"Product", s.boardProduct}, {L"Version", s.boardVersion},
                                                                    {L"Serial number", s.boardSerial}};
        std::vector<std::pair<std::wstring, std::wstring>> fw = {{L"BIOS vendor", s.biosVendor}, {L"BIOS version", s.biosVersion}, {L"BIOS date", s.biosDate},
                                                                 {L"SMBIOS version", std::to_wstring(s.majorVersion) + L"." + std::to_wstring(s.minorVersion)},
                                                                 {L"Boot mode", st.sys.bootMode}, {L"Secure Boot", st.sys.secureBoot},
                                                                 {L"TPM", st.sys.tpm + (st.sys.tpmVersion.empty() ? L"" : L" " + st.sys.tpmVersion)}};
        std::vector<std::pair<std::wstring, std::wstring>> cpu;
        for (auto& p : s.processors)
            cpu.push_back({p.socket, p.version + L" — " + std::to_wstring(p.coreCount) + L"C/" + std::to_wstring(p.threadCount) + L"T, max " + FormatFreqMHz(p.maxSpeedMHz) +
                                         (p.externalClockMHz ? L", bus " + std::to_wstring(p.externalClockMHz) + L" MHz" : L"")});
        float h1 = KvCard(ui, r.x, r.y, colW, L"System", sys);
        float h2 = KvCard(ui, r.x + colW + 16, r.y, colW, L"Baseboard", board);
        float y = r.y + std::max(h1, h2) + 16;
        float h3 = KvCard(ui, r.x, y, colW, L"Firmware", fw);
        float h4 = KvCard(ui, r.x + colW + 16, y, colW, L"Processor sockets (SMBIOS)", cpu);
        (void)h3;
        (void)h4;
    }

    void DrawSensors(Ui& ui, Rect r) {
        auto snap = app.Snap();
        auto& st = app.Static();
        sensorGrid_.rows.clear();
        uint64_t k = 1;
        auto add = [&](const std::wstring& name, const std::wstring& value, const wchar_t* status, const std::wstring& source, int hl = 0) {
            GridRow row;
            row.key = k++;
            row.cells = {name, value, status, source};
            row.highlight = hl;
            sensorGrid_.rows.push_back(std::move(row));
        };
        const wchar_t* kAvail = L"Available";
        const wchar_t* kNone = L"Unsupported";
        const wchar_t* kDriver = L"Requires kernel driver";
        const wchar_t* kAdmin = L"Requires administrator";
        const wchar_t* kEst = L"Estimated";
        if (snap) {
            add(L"CPU utilization (total and per core)", FormatPercent(snap->cpu.total, 1), kAvail, L"PDH: Processor Information \\ % Processor Utility");
            add(L"CPU effective clock", FormatFreqMHz(snap->cpu.currentMHz), kAvail, L"PDH: % Processor Performance × base clock");
            add(L"CPU throttling indicator", snap->cpu.performanceLimitPct < 99.5f ? L"Limited" : L"Not limited", kAvail, L"PDH: Processor Information \\ % Performance Limit");
            if (snap->thermalZonesC.empty()) add(L"ACPI thermal zones", L"—", kNone, L"The firmware exposes no ACPI thermal zone counters on this system", 3);
            for (auto& [n, v] : snap->thermalZonesC) add(L"ACPI thermal zone " + n, TempText(app, v), kAvail, L"PDH: Thermal Zone Information (firmware-defined location; not necessarily the CPU die)");
            add(L"CPU package / core temperatures", L"—", kDriver, L"Read from model-specific registers (MSR); needs a kernel driver. NEXUS deliberately ships none.", 3);
            add(L"CPU package power / core voltage", L"—", kDriver, L"RAPL energy counters and VID are MSRs; kernel driver required", 3);
            add(L"Motherboard voltages and fans", L"—", kDriver, L"Super I/O chips are accessed via port I/O; kernel driver required", 3);
            for (size_t i = 0; i < snap->gpus.size(); ++i) {
                auto& gpu = snap->gpus[i];
                std::wstring p = L"GPU " + std::to_wstring(i) + L" ";
                add(p + L"utilization / engines", FormatPercent(gpu.util, 1), kAvail, L"PDH: GPU Engine \\ Utilization Percentage");
                add(p + L"memory usage", FormatBytes(gpu.dedicatedUsed), kAvail, L"PDH: GPU Adapter Memory");
                add(p + L"temperature", gpu.temperatureC > 0 ? TempText(app, gpu.temperatureC) : L"—", gpu.temperatureC > 0 ? kAvail : kNone,
                    L"D3DKMT adapter performance data (driver-dependent)", gpu.temperatureC > 0 ? 0 : 3);
                add(p + L"fan speed", gpu.fanRpm > 0 ? std::to_wstring(static_cast<int>(gpu.fanRpm)) + L" RPM" : L"—", gpu.fanRpm > 0 ? kAvail : kNone,
                    L"D3DKMT adapter performance data (driver-dependent)", gpu.fanRpm > 0 ? 0 : 3);
                add(p + L"core / memory clock", gpu.coreFreqMHz > 0 ? FormatFreqMHz(gpu.coreFreqMHz) : L"—", gpu.coreFreqMHz > 0 ? kAvail : kNone,
                    L"D3DKMT node / adapter performance data (driver-dependent)", gpu.coreFreqMHz > 0 ? 0 : 3);
                add(p + L"power", gpu.powerPct > 0 ? FormatPercent(gpu.powerPct, 1) : L"—", gpu.powerPct > 0 ? kAvail : kNone,
                    L"D3DKMT adapter performance data, relative to rated power (driver-dependent)", gpu.powerPct > 0 ? 0 : 3);
                add(p + L"hotspot temperature", L"—", kNone, L"Not part of the Windows GPU interface; vendor SDK (NVAPI/ADLX) required", 3);
            }
        }
        for (auto& d : st.disks) {
            auto hi = st.health.find(d.index);
            std::wstring name = L"Disk " + std::to_wstring(d.index) + L" health / temperature";
            if (hi == st.health.end()) add(name, L"—", L"Loading", L"");
            else if (hi->second.available)
                add(name, hi->second.temperatureC > -50 ? TempText(app, hi->second.temperatureC) : L"—", kAvail, hi->second.source);
            else
                add(name, L"—", app.Elevated() ? kNone : kAdmin, hi->second.unavailableReason, 3);
        }
        add(L"Per-process network throughput", snap && snap->netPerProcess ? L"Active" : L"—", snap && snap->netPerProcess ? kAvail : kAdmin,
            L"ETW: Microsoft-Windows-Kernel-Network", snap && snap->netPerProcess ? 0 : 3);
        add(L"Process power usage", L"Low … Very high", kEst, L"NEXUS heuristic from CPU, GPU, I/O and network activity (not a wattage)");
        add(L"Memory timings", L"—", kDriver, L"SPD EEPROM on the SMBus; kernel driver required", 3);
        sensorGrid_.Refresh(sensors_);
        sensors_.Draw(ui, r, sensorGrid_);
    }

    int tab_ = 0;
    float scroll_ = 0;
    Table cores_, modules_, sensors_;
    SimpleGrid coreGrid_, modGrid_, sensorGrid_;
};

} // namespace

std::unique_ptr<Page> MakeHardwarePage(App& app) { return std::make_unique<HardwarePage>(app); }

} // namespace nx
