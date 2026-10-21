'use strict';
// Vela UI. One page, two modes:
//  - inside the Windows host (WebView2): talks to the engine over
//    chrome.webview messages; the video is a native swap chain underneath;
//  - in a plain browser: a mock engine below simulates state and telemetry,
//    so the interface can be reviewed without the hardware.
(() => {
  const $ = (s, root = document) => root.querySelector(s);
  const $$ = (s, root = document) => [...root.querySelectorAll(s)];

  // ------------------------------------------------------------------ bridge
  const webview = window.chrome && window.chrome.webview;
  const bridge = webview ? hostBridge(webview) : mockBridge();

  function hostBridge(wv) {
    const listeners = [];
    wv.addEventListener('message', (e) => {
      const msg = typeof e.data === 'string' ? JSON.parse(e.data) : e.data;
      listeners.forEach((fn) => fn(msg));
    });
    document.body.classList.add('host');
    return { demo: false, send: (m) => wv.postMessage(m), on: (fn) => listeners.push(fn) };
  }

  // ------------------------------------------------------------------- state
  const view = { settings: null, caps: null, plan: null, source: null, display: null,
                 devices: [], modes: [], system: [], tab: null, notes: '' };
  let tm = null;   // latest telemetry

  const getPath = (obj, path) => path.split('.').reduce((o, k) => (o == null ? o : o[k]), obj);
  const setPath = (obj, path, value) => {
    const keys = path.split('.');
    const last = keys.pop();
    keys.reduce((o, k) => (o[k] ??= {}), obj)[last] = value;
  };

  function set(path, value) {
    setPath(view.settings, path, value);
    bridge.send({ type: 'set', path, value });
    renderState();
  }

  const fmt = {
    'frameGen.compression': (v) => `${Math.round(v * 100)}%`,
    'superRes.sharpness': (v) => v.toFixed(2),
    'hdr.peakNits': (v) => `${v} nits`,
    'hdr.contrast': (v) => `${v}%`,
    'hdr.saturation': (v) => `${v}%`,
    'hdr.middleGray': (v) => `${v}`,
    'color.strength': (v) => `${Math.round(v * 100)}%`,
    'capture.volume': (v) => `${Math.round(v * 100)}%`,
  };
  const ms = (v, digits = 1) => (v == null || !(v >= 0) ? '—' : v.toFixed(digits));
  const fps = (v) => (v > 0 ? `${v.toFixed(v >= 100 ? 0 : 1)}` : '—');
  const res = (w, h) => (w && h ? `${w}×${h}` : '—');
  const hdrName = { passthrough: 'HDR10 直通', rtxHdr: 'RTX HDR', toneMap: '映射为 SDR', sdr: 'SDR' };
  const srName = { videoSr: 'RTX Video', dlss: 'DLSS', none: '—' };
  const latencyClass = (v) => (v < 30 ? 'lat-good' : v < 50 ? 'lat-warn' : 'lat-bad');

  // ------------------------------------------------------------- rendering
  function bindInputs() {
    const s = view.settings;
    for (const el of $$('[data-path]')) {
      const path = el.dataset.path;
      const value = getPath(s, path);
      if (el.classList.contains('seg')) {
        for (const b of $$('button', el)) b.setAttribute('aria-pressed', String(b.dataset.value === String(value)));
      } else if (el.type === 'checkbox') {
        el.checked = !!value;
      } else if (document.activeElement !== el) {
        el.value = value ?? '';
      }
      const out = el.id && $(`#${el.id}-out`);
      if (out && fmt[path]) out.textContent = fmt[path](Number(value));
    }
    for (const b of $$('[data-toggle]')) b.setAttribute('aria-pressed', String(!!getPath(s, b.dataset.toggle)));
  }

  function renderState() {
    if (!view.settings) return;
    const { settings: s, plan: p, source: src, caps } = view;
    bindInputs();

    // multipliers the plan allows; others stay visible but disabled
    const allowed = new Set(p?.multipliers ?? []);
    for (const b of $$('#fg-multiplier button')) b.disabled = !allowed.has(Number(b.dataset.value));
    const maxCap = caps?.fgMaxMultiplier ?? 0;
    $('#fg-multiplier-hint').textContent = allowed.size
      ? `可用 ${[...allowed].map((m) => `${m}×`).join(' / ')}，受显示器刷新率 ${view.display?.refreshHz ?? '—'} Hz 与驱动上限 ${maxCap}× 限制。`
      : '当前不可用，原因见下方提示。';
    $('#fg-compression-field').hidden = s.frameGen.pacing !== 'lowLatency';

    // dock
    const fgOn = s.frameGen.enabled && p && p.multiplier > 1;
    $('.dock-fg').classList.toggle('is-off', !fgOn);
    $('#dock-fg').textContent = fgOn ? `${p.multiplier}×` : '关';
    $('#dock-sr').textContent = !s.superRes.enabled ? '关' : p?.superRes !== 'none' ? srName[p.superRes] : '未启用';
    $('#dock-hdr').textContent = s.hdr.mode === 'off' && !src?.hdr ? 'SDR' : hdrName[p?.hdr ?? 'sdr'];
    $('#dock-color').textContent = s.color.enabled ? `自动 ${Math.round(s.color.strength * 100)}%` : '关';

    // signal chain
    const live = src && src.signal;
    $('#signal-dot').classList.toggle('live', !!live);
    $('#chain-device').textContent = view.devices.find((d) => d.path === s.capture.device)?.name ?? '未选择设备';
    $('#chain-in').textContent = live
      ? `${res(src.width, src.height)} · ${fps(src.fps)} Hz · ${src.hdr ? 'HDR10' : 'SDR'}`
      : '无信号';
    $('#chain-out').textContent = live && p ? `${res(p.outputWidth, p.outputHeight)} · ${fps(p.outputFps)} fps` : '—';

    // frame generation summary + facts
    $('#fg-summary').innerHTML = !s.frameGen.enabled ? '帧生成已关闭，画面按源帧率输出。'
      : p?.frameGenBlocked ? '帧生成暂不可用。'
      : `源 <strong>${fps(src?.fps)} fps</strong> → 输出 <strong>${fps(p?.outputFps)} fps</strong>，每两帧真实画面之间插入 ${p.multiplier - 1} 帧。`;
    renderFgFacts();

    // super resolution
    const scaleUp = src && view.display && (view.display.width > src.width * 1.05);
    $('#sr-summary').innerHTML = !s.superRes.enabled ? '超分已关闭，画面按源分辨率输出，由显示缩放。'
      : p?.superRes !== 'none' ? `<strong>${res(src.width, src.height)}</strong> → <strong>${res(p.outputWidth, p.outputHeight)}</strong>，使用 ${srName[p.superRes]} 超分。`
      : scaleUp ? '超分组件不可用。' : '源分辨率已不低于显示器，超分不需要工作。';

    // HDR
    const hdrLines = {
      passthrough: '信号为 HDR10，按 PQ / BT.2020 原样送到 HDR 显示器。RTX HDR 参数此时不参与。',
      rtxHdr: 'SDR 信号经 RTX Video HDR 转换后以 HDR10 输出。',
      toneMap: 'HDR 信号在 GPU 上映射为 SDR 输出。',
      sdr: 'SDR 信号，按 SDR 输出。',
    };
    $('#hdr-summary').textContent = hdrLines[p?.hdr ?? 'sdr'];
    for (const id of ['hdr-peak', 'hdr-contrast', 'hdr-sat', 'hdr-gray']) $(`#${id}`).disabled = p?.hdr !== 'rtxHdr';
    facts('#hdr-facts', [
      ['信号', src?.hdr ? `${src.colorspace}` : 'SDR BT.709'],
      ['MaxCLL / MaxFALL', src?.hdr && src.maxCll ? `${src.maxCll} / ${src.maxFall} nits` : '—'],
      ['显示器', view.display ? `${view.display.hdr ? 'HDR 开' : 'HDR 关'} · 峰值 ${view.display.peakNits ?? '—'} nits` : '—'],
    ]);

    // device lists
    fillSelect('#dev-device', view.devices.map((d) => [d.path, d.name]), s.capture.device);
    fillSelect('#dev-mode', view.modes.map((m) => [m.key, m.label]), s.capture.mode);
    $('#dev-checks').innerHTML = view.system.map((c) =>
      `<li><span class="state ${c.state}"></span><span>${esc(c.name)}</span><span class="detail">${esc(c.detail ?? '')}</span></li>`).join('');

    // HUD placement
    const hud = $('#hud');
    hud.hidden = !s.latency.hud;
    hud.dataset.corner = String(s.latency.hudCorner);

    // plan notes -> toast once per change
    const notes = (p?.notes ?? []).join('；');
    if (notes && notes !== view.notes) toast(notes);
    view.notes = notes;
  }

  function renderFgFacts() {
    const caps = view.caps;
    facts('#fg-facts', [
      ['DLSS-G 运行库', caps?.dlssgVersion ?? '—'],
      ['驱动上限', caps?.fgMaxMultiplier ? `${caps.fgMaxMultiplier}×` : '不可用'],
      ['光流', caps?.opticalFlow ? 'NVOF 硬件光流' : '不可用'],
      ['额外延迟', tm && tm.fgAddedMs > 0 ? `≈ ${ms(tm.fgAddedMs)} ms` : '—'],
    ]);
  }

  function facts(sel, rows) {
    $(sel).innerHTML = rows.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${esc(String(v))}</dd>`).join('');
  }
  function fillSelect(sel, options, value) {
    const el = $(sel);
    const html = options.map(([v, l]) => `<option value="${esc(v)}">${esc(l)}</option>`).join('');
    if (el.dataset.html !== html) { el.innerHTML = html; el.dataset.html = html; }
    if (document.activeElement !== el) el.value = value;
  }
  function esc(s) {
    return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  }

  const stageRows = [
    ['captureCopy', '采集拷贝 (CPU)'], ['upload', '上传与转换'], ['flow', '光流'], ['color', '调色'],
    ['superRes', '超分'], ['hdr', 'HDR'], ['frameGen', '帧生成'], ['output', '输出'],
  ];

  function renderTelemetry() {
    if (!tm || !view.settings) return;
    const s = view.settings;
    const cls = latencyClass(tm.endToEndMs);
    const e2e = ms(tm.endToEndMs);
    const caption = tm.calibrated ? '端到端估算（含采集卡与显示器）' : '端到端估算（采集卡延迟未校准）';

    // dock + HUD
    const dl = $('#dock-latency');
    dl.textContent = `${e2e} ms`;
    dl.className = `dock-value dock-num ${cls}`;
    if (!$('#hud').hidden) {
      const hv = $('#hud-e2e');
      hv.textContent = e2e;
      hv.className = `hud-value ${cls}`;
      $('#hud-label').textContent = tm.calibrated ? '端到端估算' : '端到端估算 · 未校准';
      $('#hud-fps').textContent = fps(tm.outputFps);
      $('#hud-src').textContent = fps(tm.sourceFps);
      $('#hud-p95').textContent = `${ms(tm.pipeline.p95)}`;
      $('#hud-gpu').textContent = `${ms(tm.gpuTotal.p50, 2)}`;
      spark($('#hud-spark'), tm.intervals.slice(-120), tm.outputFps, false);
    }

    if (view.tab === 'latency') {
      $('#lat-e2e').textContent = e2e;
      $('#lat-e2e').className = `big-num ${cls}`;
      $('#lat-caption').textContent = caption;
      const card = tm.calibrated ? s.latency.captureHardwareMs : 0;
      const copy = tm.captureCopy.p50;
      const gpu = tm.gpuTotal.p50;
      const wait = Math.max(0, tm.pipeline.p50 - copy - gpu);
      const disp = tm.scanoutMs + (s.latency.displayProcessingMs || 0);
      const parts = [
        ['采集卡', card, 'var(--seg-capture)', tm.calibrated ? null : '未校准'],
        ['CPU 拷贝', copy, 'var(--seg-capture)'],
        ['GPU 处理', gpu, 'var(--seg-gpu)'],
        [tm.fgAddedMs > 0 ? '补帧节奏等待' : '提交与等待', wait, 'var(--seg-wait)'],
        ['显示扫描', disp, 'var(--seg-display)'],
      ];
      $('#lat-breakdown').innerHTML = parts.filter((x) => x[1] > 0)
        .map(([n, v, c]) => `<span title="${n} ${v.toFixed(1)} ms" style="flex-grow:${v};background:${c}"></span>`).join('');
      $('#lat-legend').innerHTML = parts.map(([n, v, c, alt]) =>
        `<li><i style="background:${c}"></i>${n}<b>${alt ?? `${ms(v)} ms`}</b></li>`).join('');

      const seg = tm.intervals;
      spark($('#lat-spark'), seg, tm.outputFps, true);
      $('#lat-spark-min').textContent = `目标 ${ms(1000 / Math.max(tm.outputFps, 1), 2)} ms`;
      $('#lat-spark-max').textContent = `P99 ${ms(tm.presentInterval.p99, 2)} ms`;

      $('#lat-stages').innerHTML = stageRows.map(([k, label]) => {
        const st = k === 'captureCopy' ? tm.captureCopy : tm.stages[k];
        const on = st && st.p50 >= 0;
        return `<tr class="${on ? '' : 'off'}"><td>${label}</td><td>${on ? ms(st.p50, 2) : '—'}</td><td>${on ? ms(st.p95, 2) : '—'}</td></tr>`;
      }).join('') + `<tr><td>GPU 合计</td><td>${ms(tm.gpuTotal.p50, 2)}</td><td>${ms(tm.gpuTotal.p95, 2)}</td></tr>`;

      facts('#lat-counters', [
        ['管线（回调 → 上屏）', `P50 ${ms(tm.pipeline.p50)} · P95 ${ms(tm.pipeline.p95)} ms`],
        ['补帧额外延迟', tm.fgAddedMs > 0 ? `≈ ${ms(tm.fgAddedMs)} ms` : '—'],
        ['采集丢帧', String(tm.droppedSource)],
        ['跳过的生成帧', String(tm.droppedGenerated)],
        ['已呈现', `${tm.presented}（生成 ${tm.presentedGenerated}）`],
      ]);
    }
    if (view.tab === 'frameGen') renderFgFacts();
    if (view.tab === 'color') renderColor();
  }

  const meterDefs = [
    ['黑位', 'blackPoint', 0, 0.08, (v) => v.toFixed(3)],
    ['白位', 'whitePoint', 1, 0.12, (v) => v.toFixed(3)],
    ['中间调', 'gamma', 1, 0.12, (v) => v.toFixed(3)],
    ['红增益', 'gainR', 1, 0.08, (v) => v.toFixed(3)],
    ['蓝增益', 'gainB', 1, 0.08, (v) => v.toFixed(3)],
    ['饱和度', 'saturation', 1, 0.12, (v) => v.toFixed(3)],
  ];
  function renderColor() {
    const c = tm?.color;
    if (!c) return;
    $('#color-meters').innerHTML = meterDefs.map(([name, key, centre, span, f]) => {
      const d = Math.max(-1, Math.min(1, (c[key] - centre) / span));
      const left = 50 + Math.min(0, d) * 50;
      return `<div class="meter"><span class="meter-name">${name}</span><span class="meter-track"><span class="meter-fill" style="left:${left}%;width:${Math.abs(d) * 50}%"></span></span><span class="meter-value">${f(c[key])}</span></div>`;
    }).join('');
  }

  function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }
  function spark(canvas, values, outFps, detailed) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (!w || !h) return;
    if (canvas.width !== Math.round(w * dpr)) { canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr); }
    const g = canvas.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    if (!values.length) return;
    const ideal = 1000 / Math.max(outFps, 1);
    const top = Math.max(ideal * 2.2, ...values) * 1.08;
    const y = (v) => h - 4 - (v / top) * (h - 8);
    // ideal interval guide
    g.strokeStyle = cssVar('--line-strong');
    g.setLineDash([3, 4]);
    g.beginPath(); g.moveTo(0, y(ideal)); g.lineTo(w, y(ideal)); g.stroke();
    g.setLineDash([]);
    const step = w / Math.max(values.length - 1, 1);
    g.beginPath();
    values.forEach((v, i) => (i ? g.lineTo(i * step, y(v)) : g.moveTo(0, y(v))));
    if (detailed) {
      g.lineTo(w, h); g.lineTo(0, h); g.closePath();
      g.fillStyle = 'rgba(158, 214, 255, 0.08)';
      g.fill();
      g.beginPath();
      values.forEach((v, i) => (i ? g.lineTo(i * step, y(v)) : g.moveTo(0, y(v))));
    }
    g.strokeStyle = cssVar('--ice');
    g.lineWidth = 1.25;
    g.stroke();
    const last = values[values.length - 1];
    g.fillStyle = cssVar('--ice');
    g.beginPath(); g.arc(w - 1.5, y(last), 2.2, 0, Math.PI * 2); g.fill();
  }

  // ------------------------------------------------------------- drawer & UI
  function openTab(tab) {
    const drawer = $('#drawer');
    if (view.tab === tab && !drawer.hidden) return closeDrawer();
    view.tab = tab;
    drawer.hidden = false;
    $('#app').classList.add('drawer-open');
    for (const b of $$('#tabs button')) b.setAttribute('aria-selected', String(b.dataset.tab === tab));
    for (const p of $$('.panel')) p.hidden = p.dataset.panel !== tab;
    for (const d of $$('.dock-item')) d.classList.toggle('is-open', d.dataset.feature === tab);
    renderState();
    renderTelemetry();
    showUi();
  }
  function closeDrawer() {
    $('#drawer').hidden = true;
    $('#app').classList.remove('drawer-open');
    view.tab = null;
    for (const d of $$('.dock-item')) d.classList.remove('is-open');
  }

  let hideTimer = 0;
  function showUi() {
    const app = $('#app');
    if (app.dataset.ui !== 'shown') { app.dataset.ui = 'shown'; bridge.send({ type: 'ui', visible: true }); }
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => { if ($('#drawer').hidden && !$('#dock').matches(':hover')) hideUi(); }, 4000);
  }
  function hideUi() {
    clearTimeout(hideTimer);
    $('#app').dataset.ui = 'hidden';
    bridge.send({ type: 'ui', visible: false });
  }

  let toastTimer = 0;
  function toast(text) {
    const t = $('#toast');
    t.textContent = text;
    t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => (t.hidden = true), 4200);
  }

  function stepMultiplier(dir) {
    const list = view.plan?.multipliers ?? [];
    if (!list.length) return;
    const cur = view.settings.frameGen.multiplier;
    const next = dir > 0 ? list.find((m) => m > cur) : [...list].reverse().find((m) => m < cur);
    if (next) set('frameGen.multiplier', next);
    if (!view.settings.frameGen.enabled) set('frameGen.enabled', true);
  }

  function parseValue(el, raw) {
    const t = el.dataset.type;
    if (t === 'int') return parseInt(raw, 10);
    if (t === 'float') return parseFloat(raw);
    return raw;
  }

  function wire() {
    document.addEventListener('click', (e) => {
      const seg = e.target.closest('.seg button');
      if (seg && !seg.disabled) {
        const host = seg.closest('.seg');
        return set(host.dataset.path, parseValue(host, seg.dataset.value));
      }
      const tog = e.target.closest('[data-toggle]');
      if (tog) return set(tog.dataset.toggle, !getPath(view.settings, tog.dataset.toggle));
      const open = e.target.closest('[data-open]');
      if (open) return openTab(open.dataset.open);
      const tab = e.target.closest('[data-tab]');
      if (tab) return openTab(tab.dataset.tab);
      const stepBtn = e.target.closest('[data-step]');
      if (stepBtn) return stepMultiplier(Number(stepBtn.dataset.step));
      const cmd = e.target.closest('[data-command]');
      if (cmd) {
        if (cmd.dataset.command === 'hideUi') return hideUi();
        if (cmd.dataset.command === 'toggleFullscreen' && bridge.demo) {
          (document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen?.())?.catch?.(() => {});
        }
        bridge.send({ type: 'command', name: cmd.dataset.command });
      }
    });
    $('#drawer-close').addEventListener('click', closeDrawer);
    for (const el of $$('input[data-path], select[data-path]')) {
      const ev = el.type === 'range' ? 'input' : 'change';
      el.addEventListener(ev, () => {
        const v = el.type === 'checkbox' ? el.checked : parseValue(el, el.value);
        if (el.type === 'number' && Number.isNaN(v)) return;
        set(el.dataset.path, v);
      });
    }
    document.addEventListener('mousemove', showUi, { passive: true });
    document.addEventListener('keydown', (e) => {
      if (e.target.matches('input[type="number"], select')) return;
      if (e.key === 'Home') { e.preventDefault(); return $('#app').dataset.ui === 'shown' ? hideUi() : showUi(); }
      if (e.key === 'Escape') return closeDrawer();
      if (e.key === 'Enter' && e.altKey) return bridge.send({ type: 'command', name: 'toggleFullscreen' });
      if (e.key === 'f' || e.key === 'F') return set('frameGen.enabled', !view.settings.frameGen.enabled);
      if (e.key === 'l' || e.key === 'L') return set('latency.hud', !view.settings.latency.hud);
      if (/^[2-6]$/.test(e.key) && view.plan?.multipliers?.includes(Number(e.key))) set('frameGen.multiplier', Number(e.key));
    });
  }

  bridge.on((msg) => {
    if (msg.type === 'state') {
      Object.assign(view, {
        settings: msg.settings, caps: msg.caps, plan: msg.plan, source: msg.source,
        display: msg.display, devices: msg.devices ?? [], modes: msg.modes ?? [], system: msg.system ?? [],
      });
      renderState();
      renderTelemetry();
    } else if (msg.type === 'telemetry') {
      tm = msg.t;
      renderTelemetry();
    } else if (msg.type === 'toast') {
      toast(msg.text);
    } else if (msg.type === 'ui') {
      msg.visible ? showUi() : hideUi();
    }
  });

  wire();
  if (bridge.demo) { $('#demo-flag').hidden = false; startDemoPicture(); }
  bridge.send({ type: 'hello' });
  showUi();

  // ===================================================================
  // Mock engine (browser only). Mirrors the core's planner and pacing
  // arithmetic closely enough for the numbers to be plausible; the real
  // values come from vela_core in the Windows build.
  // ===================================================================
  function mockBridge() {
    const listeners = [];
    const emit = (m) => setTimeout(() => listeners.forEach((fn) => fn(structuredClone(m))), 0);
    const modes = [
      { key: 'auto', label: '自动（按信号）' },
      { key: '3840x2160@60:p010', label: '3840×2160 · 60 Hz · P010 HDR' },
      { key: '3840x2160@60:nv12', label: '3840×2160 · 60 Hz · NV12' },
      { key: '2560x1440@120:nv12', label: '2560×1440 · 120 Hz · NV12' },
      { key: '1920x1080@120:nv12', label: '1920×1080 · 120 Hz · NV12' },
      { key: '1920x1080@60:nv12', label: '1920×1080 · 60 Hz · NV12' },
    ];
    const settings = {
      frameGen: { enabled: true, multiplier: 2, pacing: 'even', compression: 0.35, reflex: true, performanceMode: false },
      latency: { hud: true, hudCorner: 1, captureHardwareMs: 0, displayProcessingMs: 0 },
      superRes: { enabled: true, quality: 4, sharpness: 0 },
      hdr: { mode: 'auto', rtxHdr: true, peakNits: 1000, contrast: 100, saturation: 100, middleGray: 50 },
      color: { enabled: true, strength: 0.5, levels: true, whiteBalance: true, vibrance: true, speed: 'normal' },
      capture: { device: 'demo-4kpro', mode: 'auto', audio: true, volume: 0.8 },
      display: { sync: 'vrr', fullscreen: true },
    };
    const caps = { gpu: 'NVIDIA GeForce RTX 5070 Ti', vramMB: 16303, fgMaxMultiplier: 6, videoSr: true, dlssSr: true,
                   trueHdr: true, opticalFlow: true, hags: true, dlssgVersion: '310.x（演示）' };
    const display = { name: '4K 240 Hz HDR', width: 3840, height: 2160, refreshHz: 240, hdr: true, vrr: true, peakNits: 1000 };
    const devices = [{ path: 'demo-4kpro', name: 'Elgato 4K Pro' }];
    const system = [
      { name: 'GPU', state: 'ok', detail: 'RTX 5070 Ti · 16 GB' },
      { name: '硬件加速 GPU 计划', state: 'ok', detail: '已开启' },
      { name: 'DLSS 帧生成运行库', state: 'ok', detail: 'nvngx_dlssg.dll' },
      { name: 'RTX Video SDK', state: 'ok', detail: 'VSR + TrueHDR' },
      { name: 'Windows HDR', state: 'ok', detail: '已开启' },
      { name: '采集卡驱动', state: 'ok', detail: 'Elgato 4K Pro' },
    ];

    function source() {
      const key = settings.capture.mode === 'auto' ? '3840x2160@60:p010' : settings.capture.mode;
      const [, w, h, f, fmtName] = key.match(/(\d+)x(\d+)@(\d+):(\w+)/);
      const hdr = fmtName === 'p010';
      return { signal: true, width: +w, height: +h, fps: +f, hdr, format: fmtName.toUpperCase(),
               colorspace: hdr ? 'HDR10 · BT.2020 PQ' : 'SDR · BT.709', maxCll: hdr ? 1000 : 0, maxFall: hdr ? 400 : 0 };
    }

    function plan(src) {
      const notes = [];
      let hdrPath = 'sdr';
      if (src.hdr) hdrPath = settings.hdr.mode === 'off' || !display.hdr ? 'toneMap' : 'passthrough';
      else if (settings.hdr.mode === 'auto' && display.hdr && settings.hdr.rtxHdr) hdrPath = 'rtxHdr';
      let outW = src.width, outH = src.height, sr = 'none';
      const scale = Math.min(display.width / src.width, display.height / src.height);
      if (settings.superRes.enabled && scale > 1.05) {
        sr = hdrPath === 'passthrough' ? 'dlss' : 'videoSr';
        outW = Math.round(src.width * scale / 2) * 2;
        outH = Math.round(src.height * scale / 2) * 2;
      }
      const capByRefresh = settings.display.sync !== 'tearing';
      const multipliers = [];
      for (let m = 2; m <= Math.min(caps.fgMaxMultiplier, 6); m++)
        if (!capByRefresh || src.fps * m <= display.refreshHz * 1.01) multipliers.push(m);
      let multiplier = 1;
      if (settings.frameGen.enabled && multipliers.length) {
        multiplier = multipliers[0];
        for (const m of multipliers) if (m <= settings.frameGen.multiplier) multiplier = m;
        if (multiplier !== settings.frameGen.multiplier) notes.push(`补帧倍数已按刷新率限制为 ${multiplier}×`);
      } else if (settings.frameGen.enabled) notes.push('显示器刷新率不足以容纳 2× 补帧');
      return { outputWidth: outW, outputHeight: outH, superRes: sr, hdr: hdrPath, multipliers, multiplier,
               outputFps: src.fps * multiplier, frameGenBlocked: settings.frameGen.enabled && multiplier < 2, notes };
    }

    function publishState() {
      const src = source();
      emit({ type: 'state', settings, caps, plan: plan(src), source: src, display, devices, modes, system });
    }

    // telemetry --------------------------------------------------------
    const intervals = [];
    const counters = { sourceFrames: 0, presented: 0, presentedGenerated: 0, droppedGenerated: 0 };
    const color = { blackPoint: 0, whitePoint: 1, gamma: 1, gainR: 1, gainG: 1, gainB: 1, saturation: 1 };
    let t0 = performance.now();
    const n = (sd = 0.05) => 1 + (Math.random() * 2 - 1) * sd;
    const pct = (base, spread) => ({ p50: base, p95: base * (1 + spread) });

    function tick() {
      const now = performance.now();
      const dt = (now - t0) / 1000;
      t0 = now;
      const src = source();
      const p = plan(src);
      const mult = p.multiplier;
      const T = 1000 / src.fps;
      const inScale = (src.width * src.height) / 8294400;
      const outScale = (p.outputWidth * p.outputHeight) / 8294400;
      const st = {
        upload: pct(0.2 * inScale * n(), 0.08),
        flow: mult > 1 ? pct(1.0 * n(), 0.1) : null,
        color: pct(0.15 * inScale * n(), 0.1),
        superRes: p.superRes === 'videoSr' ? pct(1.6 * outScale * n(), 0.07) : p.superRes === 'dlss' ? pct(1.3 * outScale * n(), 0.07) : null,
        hdr: p.hdr === 'rtxHdr' ? pct(0.8 * outScale * n(), 0.08) : p.hdr === 'toneMap' ? pct(0.1 * outScale, 0.1) : null,
        frameGen: mult > 1 ? pct((2.1 + 1.2 * (mult - 2)) * outScale * (settings.frameGen.performanceMode ? 0.5 : 1) * n(), 0.07) : null,
        output: pct(0.05 * mult, 0.1),
      };
      const gpu = Object.values(st).reduce((a, s) => a + (s ? s.p50 : 0), 0);
      const copy = 1.0 * inScale * (src.hdr ? 2 : 1) * n();
      const submit = 0.8;
      const lowLat = settings.frameGen.pacing === 'lowLatency';
      const spacing = (T / mult) * (lowLat ? 1 - settings.frameGen.compression : 1);
      const pipeline = mult > 1 ? copy + submit + gpu * 1.07 + 1 + (mult - 1) * spacing + 0.2 : copy + submit + gpu + 0.2;
      const scanout = 500 / display.refreshHz;
      const card = settings.latency.captureHardwareMs;

      // present intervals for the last tick
      const count = Math.round(p.outputFps * dt);
      for (let i = 0; i < count; i++) {
        const k = i % mult;
        let v = mult > 1 && lowLat ? (k === mult - 1 ? T - (mult - 1) * spacing : spacing) : T / mult;
        intervals.push(Math.max(0.3, v + (Math.random() - 0.5) * 0.25));
      }
      while (intervals.length > 960) intervals.shift();
      counters.sourceFrames += Math.round(src.fps * dt);
      counters.presented += count;
      counters.presentedGenerated += Math.round(count * (mult - 1) / mult);

      // auto colour drifts slowly around a mild correction of the demo picture
      const goal = settings.color.enabled ? settings.color.strength : 0;
      const wob = Math.sin(now / 2400) * 0.3 + 1;
      const target = { blackPoint: 0.035 * goal * wob, whitePoint: 1 - 0.05 * goal, gamma: 1 - 0.06 * goal * wob,
                       gainR: 1 + 0.025 * goal, gainG: 1 - 0.004 * goal, gainB: 1 - 0.04 * goal * wob, saturation: 1 + 0.06 * goal };
      const a = 1 - Math.exp(-dt / ({ slow: 2, normal: 0.8, fast: 0.3 }[settings.color.speed] ?? 0.8));
      for (const k in color) color[k] += (target[k] - color[k]) * a;

      const sorted = intervals.slice(-240).sort((x, y) => x - y);
      emit({ type: 'telemetry', t: {
        sourceFps: src.fps * n(0.002), outputFps: p.outputFps * n(0.002),
        pipeline: pct(pipeline, 0.02), captureCopy: pct(copy, 0.12), gpuTotal: pct(gpu, 0.06),
        presentInterval: { p50: sorted[Math.floor(sorted.length * 0.5)] ?? 0, p99: sorted[Math.floor(sorted.length * 0.99)] ?? 0 },
        stages: st, scanoutMs: scanout,
        endToEndMs: card + pipeline + scanout + settings.latency.displayProcessingMs,
        fgAddedMs: mult > 1 ? ((mult - 1) / mult) * T + st.frameGen.p50 : 0,
        calibrated: card > 0, droppedSource: 0, ...counters, intervals: intervals.slice(),
        color: { ...color },
      } });
    }

    return {
      demo: true,
      on: (fn) => listeners.push(fn),
      send(msg) {
        if (msg.type === 'hello') { publishState(); setInterval(tick, 100); }
        else if (msg.type === 'set') {
          setPath(settings, msg.path, msg.value);
          if (msg.path === 'frameGen.multiplier' || msg.path.startsWith('capture.') || msg.path.startsWith('display.') ||
              msg.path.startsWith('hdr.') || msg.path.startsWith('superRes.') || msg.path === 'frameGen.enabled') publishState();
        } else if (msg.type === 'command' && msg.name === 'resetStats') {
          intervals.length = 0;
          Object.keys(counters).forEach((k) => (counters[k] = 0));
        }
      },
    };
  }

  // Demo picture: a dusk landscape scrolling sideways. With frame generation
  // off it advances only at the source rate, so the difference is visible on
  // a high refresh display. Purely illustrative.
  function startDemoPicture() {
    const canvas = $('#demo-picture');
    const g = canvas.getContext('2d');
    let lastStep = 0, shown = 0;
    const layers = [
      { speed: 18, base: 0.62, amp: 0.10, freq: 1.3, color: '#2a2f4a' },
      { speed: 45, base: 0.70, amp: 0.08, freq: 2.1, color: '#1d2034' },
      { speed: 110, base: 0.80, amp: 0.05, freq: 3.4, color: '#12131f' },
    ];
    function frame(now) {
      requestAnimationFrame(frame);
      const s = view.settings, src = view.source, p = view.plan;
      const fgOn = s && s.frameGen.enabled && p && p.multiplier > 1;
      const srcFps = src?.fps || 60;
      const t = now / 1000;
      if (!fgOn && t - lastStep < 1 / srcFps - 0.0005) return;
      lastStep = fgOn ? t : Math.floor(t * srcFps) / srcFps;
      shown = lastStep;
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const w = canvas.clientWidth, h = canvas.clientHeight;
      if (canvas.width !== Math.round(w * dpr)) { canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr); }
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      const sky = g.createLinearGradient(0, 0, 0, h);
      sky.addColorStop(0, '#141a33'); sky.addColorStop(0.55, '#5b3f63'); sky.addColorStop(0.8, '#d98a5f');
      g.fillStyle = sky; g.fillRect(0, 0, w, h);
      g.fillStyle = 'rgba(255, 214, 160, 0.9)';
      g.beginPath(); g.arc(w * 0.68, h * 0.6, Math.min(w, h) * 0.07, 0, Math.PI * 2); g.fill();
      for (const L of layers) {
        g.fillStyle = L.color;
        g.beginPath(); g.moveTo(0, h);
        for (let x = 0; x <= w + 8; x += 8) {
          const u = (x + shown * L.speed * (w / 1000) * 10) / w;
          const y = h * (L.base - L.amp * (Math.sin(u * L.freq * 6.283) * 0.6 + Math.sin(u * L.freq * 2.7 * 6.283 + 1.3) * 0.4));
          g.lineTo(x, y);
        }
        g.lineTo(w, h); g.closePath(); g.fill();
      }
      // fence posts: fast motion makes the frame rate visible
      g.fillStyle = '#0a0b12';
      const gap = 120, off = (shown * 900) % gap;
      for (let x = -off; x < w; x += gap) g.fillRect(x, h * 0.86, 10, h * 0.14);
      g.fillRect(0, h * 0.93, w, h * 0.07);
    }
    requestAnimationFrame(frame);
  }
})();
