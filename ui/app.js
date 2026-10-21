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
    // A preset overwrites the fine-tune amounts (the host does the same).
    if (path === 'color.preset') Object.assign(view.settings.color, view.caps?.colorPresets?.[value]);
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
    'color.levels': (v) => `${Math.round(v * 100)}%`,
    'color.highlights': (v) => `${Math.round(v * 100)}%`,
    'color.shadows': (v) => `${Math.round(v * 100)}%`,
    'color.saturation': (v) => (Math.abs(v) < 0.005 ? '0' : `${v > 0 ? '+' : '−'}${Math.round(Math.abs(v) * 100)}%`),
    'color.whiteBalance': (v) => `${Math.round(v * 100)}%`,
    'capture.volume': (v) => `${Math.round(v * 100)}%`,
  };
  const contentName = { auto: '自动', 30: '30 fps', 60: '60 fps', 120: '120 fps' };
  const ms = (v, digits = 1) => (v == null || !(v >= 0) ? '—' : v.toFixed(digits));
  const fps = (v) => (!(v > 0) ? '—' : Math.abs(v - Math.round(v)) < 0.05 || v >= 100 ? `${Math.round(v)}` : v.toFixed(1));
  const res = (w, h) => (w && h ? `${w}×${h}` : '—');
  const hdrName = { passthrough: 'HDR10 直通', rtxHdr: 'RTX HDR', toneMap: '映射为 SDR', sdr: 'SDR' };
  const srName = { videoSr: 'RTX Video', dlss: 'DLSS', none: '—' };
  const presetName = { natural: '自然', washed: '去灰', overexposed: '压高光', oversaturated: '降饱和', dark: '提亮暗部', cast: '去偏色' };
  const issueName = { washed: '发灰', overexposed: '过曝', oversaturated: '过饱和', dark: '偏暗', cast: '偏色' };
  const amountKeys = ['levels', 'highlights', 'shadows', 'saturation', 'whiteBalance'];
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

  const app = $('#app');
  const isFull = () => app.dataset.mode === 'fullscreen';
  const railHdr = { passthrough: 'HDR', rtxHdr: 'RTX', toneMap: 'SDR', sdr: 'SDR' };

  function renderState() {
    if (!view.settings) return;
    const { settings: s, plan: p, source: src, caps } = view;
    bindInputs();

    // multipliers the plan allows; others stay visible but disabled
    const allowed = new Set(p?.multipliers ?? []);
    for (const b of $$('#fg-multiplier button')) b.disabled = !allowed.has(Number(b.dataset.value));
    const maxCap = caps?.fgMaxMultiplier ?? 0;
    const fgOn = s.frameGen.enabled && p && p.multiplier > 1;
    $('#fg-formula').textContent = fgOn ? `${fps(p.contentFps)} × ${p.multiplier} = ${fps(p.outputFps)} fps` : '';
    $('#fg-multiplier-hint').textContent = allowed.size
      ? `按内容帧率计算。可用 ${[...allowed].map((m) => `${m}×`).join(' / ')}，受显示器 ${view.display?.refreshHz ?? '—'} Hz 与驱动上限 ${maxCap}× 限制。`
      : '当前不可用，原因见提示。';
    $('#fg-compression-field').hidden = s.frameGen.pacing !== 'lowLatency';

    // signal strip (windowed)
    const live = !!(src && src.signal);
    $('#tally').classList.toggle('live', live);
    $('#tally-text').textContent = live ? 'LIVE' : '无信号';
    $('#sig-device').textContent = view.devices.find((d) => d.path === s.capture.device)?.name ?? '未选择设备';
    $('#sig-in').textContent = live
      ? `${res(src.width, src.height)}p${fps(src.fps)} · ${src.hdr ? 'HDR10 PQ · BT.2020' : 'SDR · BT.709'}` : '—';
    const slower = live && p?.contentFps && p.contentFps < src.fps - 0.5;
    $('#sig-content').hidden = !slower;
    $('#sig-content').textContent = slower ? `内容 ${fps(p.contentFps)} fps` : '';
    $('#sig-out').textContent = live && p
      ? `输出 ${res(p.outputWidth, p.outputHeight)} · ${fps(p.outputFps)} fps${s.display.sync === 'vrr' ? ' · VRR' : ''}` : '—';
    $('#sig-gpu').textContent = caps?.gpu ?? '';
    $('#nosignal').hidden = live;

    // rail (fullscreen)
    $('#rail-fg').textContent = fgOn ? `${p.multiplier}×` : '关';
    $('#rail-sr').textContent = !s.superRes.enabled ? '关' : p?.superRes === 'videoSr' ? 'VSR' : p?.superRes === 'dlss' ? 'DLSS' : '原生';
    $('#rail-hdr').textContent = railHdr[p?.hdr ?? 'sdr'];
    $('#rail-color').textContent = s.color.enabled ? presetName[s.color.preset] ?? '—' : '关';
    $('[data-open="frameGen"].rail-item').classList.toggle('is-off', !fgOn);
    $('[data-open="superRes"].rail-item').classList.toggle('is-off', !s.superRes.enabled || p?.superRes === 'none');
    $('[data-open="color"].rail-item').classList.toggle('is-off', !s.color.enabled);
    $('#pill').hidden = !s.latency.hud;
    $('#pill').dataset.corner = s.latency.hudCorner;

    // frame generation summary + facts
    $('#fg-summary').innerHTML = !s.frameGen.enabled ? '帧生成已关闭，画面按源帧率输出。'
      : p?.frameGenBlocked ? '帧生成暂不可用。'
      : `内容 <strong>${fps(p?.contentFps)} fps</strong> → 输出 <strong>${fps(p?.outputFps)} fps</strong>，每两张不同的画面之间生成 ${p.multiplier - 1} 帧。`;
    renderFgFacts();
    renderColorAdvice();

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
      ['补帧额外延迟', tm && tm.fgAddedMs > 0 ? `≈ ${ms(tm.fgAddedMs)} ms` : '—'],
      ['跳过的重复画面', tm ? String(tm.repeats ?? 0) : '—'],
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

  // Where the end-to-end estimate goes, in order.
  function latencyParts() {
    const s = view.settings;
    const copy = tm.captureCopy.p50, gpu = tm.gpuTotal.p50;
    return [
      ['采集卡', tm.calibrated ? s.latency.captureHardwareMs : 0, 'var(--seg-capture)', tm.calibrated ? null : '未校准'],
      ['采集拷贝', copy, 'var(--seg-capture)'],
      ['GPU 处理', gpu, 'var(--seg-gpu)'],
      [tm.fgAddedMs > 0 ? '补帧节奏等待' : '提交与等待', Math.max(0, tm.pipeline.p50 - copy - gpu), 'var(--seg-wait)'],
      ['显示扫描', tm.scanoutMs + (s.latency.displayProcessingMs || 0), 'var(--seg-display)'],
    ];
  }
  function bar(sel, parts) {
    $(sel).innerHTML = parts.filter((x) => x[1] > 0)
      .map(([n, v, c]) => `<span title="${n} ${v.toFixed(1)} ms" style="flex-grow:${v};background:${c}"></span>`).join('');
  }

  function renderTelemetry() {
    if (!tm || !view.settings) return;
    const cls = latencyClass(tm.endToEndMs);
    const e2e = ms(tm.endToEndMs);
    const parts = latencyParts();

    const rl = $('#rail-latency');
    rl.textContent = tm.endToEndMs > 0 ? String(Math.round(tm.endToEndMs)) : '—';
    rl.className = `rail-num ${cls}`;
    if (!$('#pill').hidden) {
      $('#pill-e2e').textContent = `${e2e} ms`;
      $('#pill-e2e').className = cls;
      $('#pill-fps').textContent = `${fps(tm.outputFps)} / ${fps(tm.contentFps || tm.sourceFps)} fps`;
    }

    if (!isFull()) {
      $('#scope-e2e').textContent = e2e;
      $('#scope-e2e').className = cls;
      $('#scope-label').textContent = tm.calibrated ? '端到端 · 已校准' : '端到端 · 采集卡未校准';
      bar('#scope-bar', parts);
      $('#scope-rows').innerHTML = parts.filter((x) => x[3] === undefined || x[1] > 0)
        .map(([n, v]) => `<dt>${n}</dt><dd>${ms(v)}</dd>`).join('');
      spark($('#strip-chart'), tm.intervals, tm.outputFps);
      $('#strip-stats').textContent =
        `目标 ${ms(1000 / Math.max(tm.outputFps, 1), 2)} ms · P99 ${ms(tm.presentInterval.p99, 2)} ms · 跳过生成帧 ${tm.droppedGenerated}`;
    }

    if (view.tab === 'latency') {
      if (isFull()) {
        $('#lat-e2e').textContent = e2e;
        $('#lat-e2e').className = `big-num ${cls}`;
        $('#lat-caption').textContent = tm.calibrated ? '端到端估算（含采集卡与显示器）' : '端到端估算（采集卡延迟未校准）';
        bar('#lat-bar', parts);
        $('#lat-legend').innerHTML = parts.map(([n, v, c, alt]) =>
          `<li><i style="background:${c}"></i>${n}<b>${alt ?? ms(v)}</b></li>`).join('');
      }
      $('#lat-stages').innerHTML = stageRows.map(([k, label]) => {
        const st = k === 'captureCopy' ? tm.captureCopy : tm.stages[k];
        const on = st && st.p50 >= 0;
        return `<tr class="${on ? '' : 'off'}"><td>${label}</td><td>${on ? ms(st.p50, 2) : '—'}</td><td>${on ? ms(st.p95, 2) : '—'}</td></tr>`;
      }).join('') + `<tr><td>GPU 合计</td><td>${ms(tm.gpuTotal.p50, 2)}</td><td>${ms(tm.gpuTotal.p95, 2)}</td></tr>`;
      facts('#lat-counters', [
        ['管线（回调 → 上屏）', `${ms(tm.pipeline.p50)} / ${ms(tm.pipeline.p95)} ms`],
        ['信号 / 内容 / 输出', `${fps(tm.signalFps)} / ${fps(tm.contentFps || tm.sourceFps)} / ${fps(tm.outputFps)}`],
        ['采集丢帧', String(tm.droppedSource)],
        ['跳过的生成帧', String(tm.droppedGenerated)],
      ]);
    }
    if (view.tab === 'frameGen') {
      renderFgFacts();
      const mode = view.settings.frameGen.contentRate;
      $('#fg-detected').textContent = tm.detectedFps > 0
        ? `检测到 ${fps(tm.detectedFps)} fps${mode !== 'auto' ? `（手动 ${contentName[mode]}）` : ''}` : '检测中…';
    }
    if (view.tab === 'color') { renderColor(); renderColorAdvice(); }
  }

  // Preset state, fine-tune availability and the diagnosis banner.
  function renderColorAdvice() {
    const c = view.settings.color;
    const base = view.caps?.colorPresets?.[c.preset];
    $('#color-tuned').hidden = !base || amountKeys.every((k) => Math.abs(c[k] - base[k]) < 0.005);
    const hdr = !!view.source?.hdr;
    $('#color-hdr-hint').hidden = !hdr;
    for (const f of $$('[data-panel="color"] .sdr-only')) {
      f.classList.toggle('is-off', hdr);
      $('input', f).disabled = hdr;
    }
    const d = tm?.diagnosis;
    const rec = d?.recommended;
    for (const b of $$('#color-presets button')) b.classList.toggle('is-recommended', c.enabled && b.dataset.value === rec);
    const banner = $('#color-diag');
    banner.hidden = !c.enabled || !d;
    if (banner.hidden) return;
    const issue = issueName[rec];
    const using = rec === c.preset;
    $('#color-diag-text').innerHTML = !issue
      ? `没有明显问题${using ? '' : ' · 推荐「自然」'}`
      : `<strong>${issue}</strong>${using ? ` · 正在用「${presetName[rec]}」` : ` → 推荐「${presetName[rec]}」`}`;
    banner.classList.toggle('is-quiet', using);
    const apply = $('#color-diag-apply');
    apply.hidden = using;
    apply.dataset.preset = rec;
  }

  const meterDefs = [
    ['黑位', 'blackPoint', 0, 0.08, (v) => v.toFixed(3)],
    ['白位', 'whitePoint', 1, 0.12, (v) => v.toFixed(3)],
    ['中间调', 'gamma', 1, 0.12, (v) => v.toFixed(3)],
    ['暗部', 'lift', 0, 0.16, (v) => (v > 5e-4 ? `+${v.toFixed(3)}` : '0.000')],
    ['高光', 'shoulder', 0, -0.16, (v) => (v > 5e-4 ? `−${v.toFixed(3)}` : '0.000')],
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

  function token(name) { return getComputedStyle(app).getPropertyValue(name).trim(); }
  function spark(canvas, values, outFps) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (!w || !h) return;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    const g = canvas.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    if (!values.length) return;
    const ideal = 1000 / Math.max(outFps, 1);
    const top = Math.max(ideal * 2.2, ...values) * 1.08;
    const y = (v) => h - 3 - (v / top) * (h - 6);
    g.strokeStyle = token('--line-2');
    g.setLineDash([4, 5]);
    g.beginPath(); g.moveTo(0, y(ideal)); g.lineTo(w, y(ideal)); g.stroke();
    g.setLineDash([]);
    const step = w / Math.max(values.length - 1, 1);
    const path = () => { g.beginPath(); values.forEach((v, i) => (i ? g.lineTo(i * step, y(v)) : g.moveTo(0, y(v)))); };
    path();
    g.lineTo(w, h); g.lineTo(0, h); g.closePath();
    g.fillStyle = 'rgba(158, 214, 255, 0.07)';
    g.fill();
    path();
    g.strokeStyle = token('--data');
    g.lineWidth = 1.4;
    g.stroke();
    g.fillStyle = token('--data');
    g.beginPath(); g.arc(w - 2, y(values[values.length - 1]), 2.4, 0, Math.PI * 2); g.fill();
  }

  // ------------------------------------------------------- panels and modes
  function selectPanel(tab) {
    view.tab = tab;
    for (const b of $$('#tabs button')) b.setAttribute('aria-selected', String(b.dataset.tab === tab));
    for (const p of $$('.panel')) p.hidden = p.dataset.panel !== tab;
    const panel = $(`.panel[data-panel="${tab}"]`);
    $('#panel-title').textContent = panel ? panel.dataset.title : '';
    for (const r of $$('.rail-item')) r.classList.toggle('is-open', isFull() && app.dataset.open === r.dataset.open);
    renderState();
    renderTelemetry();
  }
  function openPanel(tab) {
    if (isFull()) {
      if (app.dataset.open === tab) return closePanel();
      app.dataset.open = tab;
    } else if (app.dataset.side === 'collapsed') {
      setSide(true);
    }
    selectPanel(tab);
    showUi();
  }
  function closePanel() {
    delete app.dataset.open;
    for (const r of $$('.rail-item')) r.classList.remove('is-open');
    showUi();
  }

  // Windowed: the scope column can be folded away for a bigger picture.
  function setSide(open) {
    app.dataset.side = open ? 'open' : 'collapsed';
    $('#side-toggle').setAttribute('aria-label', open ? '收起面板' : '展开面板');
    try { localStorage.setItem('vela.side', app.dataset.side); } catch { /* storage may be unavailable */ }
    reportViewport();
  }

  function setMode(full) {
    app.dataset.mode = full ? 'fullscreen' : 'windowed';
    delete app.dataset.open;
    if (!full) app.dataset.ui = 'shown';
    selectPanel(view.tab ?? 'frameGen');
    showUi();
    reportViewport();
  }

  // The host letterboxes the video into this rectangle (device pixels).
  let lastViewport = '';
  function reportViewport() {
    const r = $('#viewport').getBoundingClientRect();
    const d = window.devicePixelRatio || 1;
    const msg = { type: 'viewport', x: Math.round(r.left * d), y: Math.round(r.top * d), w: Math.round(r.width * d), h: Math.round(r.height * d) };
    const key = `${msg.x},${msg.y},${msg.w},${msg.h}`;
    if (key === lastViewport || !msg.w || !msg.h) return;
    lastViewport = key;
    bridge.send(msg);
  }

  // Fullscreen only: the rail fades after 4 s without input; the host then
  // hides the whole overlay so the video gets independent flip.
  let hideTimer = 0;
  function showUi() {
    if (app.dataset.ui !== 'shown') { app.dataset.ui = 'shown'; bridge.send({ type: 'ui', visible: true }); }
    clearTimeout(hideTimer);
    if (!isFull()) return;
    hideTimer = setTimeout(() => {
      if (!app.dataset.open && !$('#rail').matches(':hover') && view.source?.signal !== false) hideUi();
    }, 4000);
  }
  function hideUi() {
    clearTimeout(hideTimer);
    if (!isFull()) return;
    app.dataset.ui = 'hidden';
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

  function parseValue(el, raw) {
    const t = el.dataset.type;
    if (t === 'int') return parseInt(raw, 10);
    if (t === 'float') return parseFloat(raw);
    return raw;
  }

  function toggleFullscreen() {
    if (bridge.demo) {
      // The browser demo switches layout itself; real fullscreen is optional.
      const full = !isFull();
      try {
        if (full) document.documentElement.requestFullscreen?.()?.catch?.(() => {});
        else if (document.fullscreenElement) document.exitFullscreen()?.catch?.(() => {});
      } catch { /* not allowed in this frame */ }
      setMode(full);
    }
    bridge.send({ type: 'command', name: 'toggleFullscreen' });
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
      if (open) return openPanel(open.dataset.open);
      const tab = e.target.closest('[data-tab]');
      if (tab) return selectPanel(tab.dataset.tab);
      const cmd = e.target.closest('[data-command]');
      if (cmd) {
        if (cmd.dataset.command === 'toggleFullscreen') return toggleFullscreen();
        bridge.send({ type: 'command', name: cmd.dataset.command });
      }
    });
    $('#panel-close').addEventListener('click', closePanel);
    $('#color-diag-apply').addEventListener('click', (e) => set('color.preset', e.currentTarget.dataset.preset));
    $('#color-restore').addEventListener('click', () => set('color.preset', view.settings.color.preset));
    $('#side-toggle').addEventListener('click', () => setSide(app.dataset.side === 'collapsed'));
    for (const el of $$('input[data-path], select[data-path]')) {
      const ev = el.type === 'range' ? 'input' : 'change';
      el.addEventListener(ev, () => {
        const v = el.type === 'checkbox' ? el.checked : parseValue(el, el.value);
        if (el.type === 'number' && Number.isNaN(v)) return;
        set(el.dataset.path, v);
      });
    }
    document.addEventListener('mousemove', showUi, { passive: true });
    window.addEventListener('resize', reportViewport);
    if (window.ResizeObserver) new ResizeObserver(reportViewport).observe($('#viewport'));
    document.addEventListener('keydown', (e) => {
      if (e.target.matches('input[type="number"], select')) return;
      if (e.key === 'Home') {
        e.preventDefault();
        if (!isFull()) return setSide(app.dataset.side === 'collapsed');
        return app.dataset.ui === 'shown' ? hideUi() : showUi();
      }
      if (e.key === 'Escape') {
        if (isFull() && app.dataset.open) return closePanel();
        if (isFull()) return toggleFullscreen();
        return;
      }
      if (e.key === 'Enter' && e.altKey) return toggleFullscreen();
      if (e.key === 'f' || e.key === 'F') return set('frameGen.enabled', !view.settings.frameGen.enabled);
      if (e.key === 'l' || e.key === 'L') return set('latency.hud', !view.settings.latency.hud);
      if (e.key === 'm' || e.key === 'M') return set('capture.muted', !view.settings.capture.muted);
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
    } else if (msg.type === 'mode') {
      if (msg.fullscreen !== isFull()) setMode(!!msg.fullscreen);
    } else if (msg.type === 'toast') {
      toast(msg.text);
    } else if (msg.type === 'flash') {
      // Hotkey feedback while playing: notice only, then out of the way.
      clearTimeout(hideTimer);
      app.dataset.ui = 'flash';
      toast(msg.text);
      hideTimer = setTimeout(() => { if (app.dataset.ui === 'flash') hideUi(); }, 1500);
    } else if (msg.type === 'ui') {
      msg.visible ? showUi() : hideUi();
    }
  });

  wire();
  try { if (localStorage.getItem('vela.side') === 'collapsed') app.dataset.side = 'collapsed'; } catch { /* no storage */ }
  if (bridge.demo) { $('#demo-flag').hidden = false; startDemoPicture(); }
  selectPanel('frameGen');
  bridge.send({ type: 'hello' });
  showUi();
  reportViewport();

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
      frameGen: { enabled: true, multiplier: 4, contentRate: 'auto', pacing: 'even', compression: 0.35, reflex: true, performanceMode: false },
      latency: { hud: true, hudCorner: 1, captureHardwareMs: 0, displayProcessingMs: 0 },
      superRes: { enabled: true, quality: 4, sharpness: 0 },
      hdr: { mode: 'auto', rtxHdr: true, peakNits: 1000, contrast: 100, saturation: 100, middleGray: 50 },
      color: { enabled: true, preset: 'natural', strength: 1, levels: 0.5, highlights: 0.3, shadows: 0.2, saturation: 0.3, whiteBalance: 0.5, speed: 'normal' },
      capture: { device: 'demo-4kpro', mode: 'auto', audio: true, volume: 0.8, muted: false },
      display: { sync: 'vrr', fullscreen: true },
    };
    const caps = { gpu: 'NVIDIA GeForce RTX 5070 Ti', vramMB: 16303, fgMaxMultiplier: 6, videoSr: true, dlssSr: true,
                   trueHdr: true, opticalFlow: true, hags: true, dlssgVersion: '310.x（演示）',
                   // same table as vela::core::colorPresetAmounts
                   colorPresets: {
                     natural: { levels: 0.5, highlights: 0.3, shadows: 0.2, saturation: 0.3, whiteBalance: 0.5 },
                     washed: { levels: 1, highlights: 0.2, shadows: 0, saturation: 0.5, whiteBalance: 0.5 },
                     overexposed: { levels: 0.5, highlights: 1, shadows: 0, saturation: 0, whiteBalance: 0.4 },
                     oversaturated: { levels: 0.4, highlights: 0.3, shadows: 0.1, saturation: -1, whiteBalance: 0.4 },
                     dark: { levels: 0.5, highlights: 0, shadows: 1, saturation: 0.3, whiteBalance: 0.4 },
                     cast: { levels: 0.4, highlights: 0.2, shadows: 0.1, saturation: 0.2, whiteBalance: 1 },
                   } };
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

    // The demo game renders at 30 fps in 60 Hz signals and 60 fps in 120 Hz ones.
    const detected = (src) => (src.fps <= 60 ? 30 : 60);
    function contentFps(src) {
      const m = settings.frameGen.contentRate;
      return Math.min(m === 'auto' ? detected(src) : Number(m), src.fps);
    }
    function plan(src) {
      const notes = [];
      const content = contentFps(src);
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
        if (!capByRefresh || content * m <= display.refreshHz * 1.01) multipliers.push(m);
      let multiplier = 1;
      if (settings.frameGen.enabled && multipliers.length) {
        multiplier = multipliers[0];
        for (const m of multipliers) if (m <= settings.frameGen.multiplier) multiplier = m;
        if (multiplier !== settings.frameGen.multiplier) notes.push(`补帧倍数已按刷新率限制为 ${multiplier}×`);
      } else if (settings.frameGen.enabled) notes.push('显示器刷新率不足以容纳 2× 补帧');
      return { outputWidth: outW, outputHeight: outH, superRes: sr, hdr: hdrPath, multipliers, multiplier, contentFps: content,
               outputFps: content * multiplier, frameGenBlocked: settings.frameGen.enabled && multiplier < 2, notes };
    }

    function publishState() {
      const src = source();
      emit({ type: 'state', settings, caps, plan: plan(src), source: src, display, devices, modes, system });
    }

    // telemetry --------------------------------------------------------
    const intervals = [];
    const counters = { sourceFrames: 0, presented: 0, presentedGenerated: 0, droppedGenerated: 0, repeats: 0 };
    const color = { blackPoint: 0, whitePoint: 1, gamma: 1, lift: 0, shoulder: 0, gainR: 1, gainG: 1, gainB: 1, saturation: 1 };
    // The demo "game" moves through a scene with a typical capture problem
    // every 30 s; values are how strongly each problem shows (0..1).
    const scenes = [
      { washed: 0.78, overexposed: 0.05, oversaturated: 0.1, dark: 0.1, cast: 0.3, flat: 0.5, loud: 0 },
      { washed: 0.1, overexposed: 0.82, oversaturated: 0.15, dark: 0, cast: 0.2, flat: 0.2, loud: 0.1 },
      { washed: 0.1, overexposed: 0.1, oversaturated: 0.2, dark: 0.2, cast: 0.25, flat: 0.2, loud: 0.15 },
      { washed: 0.05, overexposed: 0.1, oversaturated: 0.85, dark: 0.1, cast: 0.2, flat: 0, loud: 0.85 },
      { washed: 0.05, overexposed: 0, oversaturated: 0.1, dark: 0.8, cast: 0.3, flat: 0.3, loud: 0 },
      { washed: 0.1, overexposed: 0.1, oversaturated: 0.2, dark: 0.1, cast: 0.8, flat: 0.2, loud: 0.1 },
    ];
    const diagnosis = { scores: null, recommended: 'natural' };
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
      const T = 1000 / p.contentFps;
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
      counters.sourceFrames += Math.round(p.contentFps * dt);
      counters.repeats += mult > 1 ? Math.round((src.fps - p.contentFps) * dt) : 0;
      counters.presented += count;
      counters.presentedGenerated += Math.round(count * (mult - 1) / mult);

      // auto colour: a rough stand-in for vela::core::AutoColor on the demo scenes
      const c = settings.color;
      const sc = { ...scenes[Math.floor(now / 30000) % scenes.length] };
      if (src.hdr) sc.washed = sc.overexposed = sc.dark = 0;   // tone problems are judged on SDR only
      const wob = Math.sin(now / 2400) * 0.1 + 1;
      const on = c.enabled ? c.strength : 0;
      const wb = 0.8 * c.whiteBalance * sc.cast * on * wob;
      const target = {
        blackPoint: 0.06 * sc.washed * c.levels * on, whitePoint: 1 - 0.1 * sc.washed * c.levels * on,
        gamma: 1 + 0.1 * (sc.overexposed - sc.dark) * c.levels * on,
        lift: 0.16 * sc.dark * c.shadows * on, shoulder: 0.16 * sc.overexposed * c.highlights * on,
        gainR: 1 + 0.05 * wb, gainG: 1 - 0.005 * wb, gainB: 1 - 0.08 * wb,
        saturation: 1 + on * (c.saturation >= 0 ? 0.15 * sc.flat * c.saturation : c.saturation * (0.05 + 0.15 * sc.loud)),
      };
      const a = 1 - Math.exp(-dt / ({ slow: 2, normal: 0.8, fast: 0.3 }[c.speed] ?? 0.8));
      for (const k in color) color[k] += (target[k] - color[k]) * a;
      // diagnosis: 2 s smoothing, Natural is the bar, 0.1 hysteresis
      const instant = { natural: 0.5, washed: sc.washed, overexposed: sc.overexposed, oversaturated: sc.oversaturated, dark: sc.dark, cast: sc.cast };
      if (!diagnosis.scores) diagnosis.scores = instant;
      const ad = 1 - Math.exp(-dt / 2);
      for (const k in instant) diagnosis.scores[k] += (instant[k] - diagnosis.scores[k]) * ad;
      const best = Object.keys(diagnosis.scores).reduce((x, y) => (diagnosis.scores[y] > diagnosis.scores[x] ? y : x));
      if (diagnosis.scores[best] > diagnosis.scores[diagnosis.recommended] + 0.1) diagnosis.recommended = best;

      const sorted = intervals.slice(-240).sort((x, y) => x - y);
      emit({ type: 'telemetry', t: {
        signalFps: src.fps * n(0.002), sourceFps: p.contentFps * n(0.002), outputFps: p.outputFps * n(0.002),
        contentFps: p.contentFps, detectedFps: detected(src),
        pipeline: pct(pipeline, 0.02), captureCopy: pct(copy, 0.12), gpuTotal: pct(gpu, 0.06),
        presentInterval: { p50: sorted[Math.floor(sorted.length * 0.5)] ?? 0, p99: sorted[Math.floor(sorted.length * 0.99)] ?? 0 },
        stages: st, scanoutMs: scanout,
        endToEndMs: card + pipeline + scanout + settings.latency.displayProcessingMs,
        fgAddedMs: mult > 1 ? ((mult - 1) / mult) * T + st.frameGen.p50 : 0,
        calibrated: card > 0, droppedSource: 0, ...counters, intervals: intervals.slice(),
        color: { ...color }, diagnosis: structuredClone(diagnosis),
      } });
    }

    return {
      demo: true,
      on: (fn) => listeners.push(fn),
      send(msg) {
        if (msg.type === 'hello') { publishState(); setInterval(tick, 100); }
        else if (msg.type === 'set') {
          setPath(settings, msg.path, msg.value);
          if (msg.path === 'color.preset') Object.assign(settings.color, caps.colorPresets[msg.value]);
          if (msg.path === 'frameGen.multiplier' || msg.path === 'frameGen.contentRate' || msg.path.startsWith('capture.') || msg.path.startsWith('display.') ||
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
      const srcFps = p?.contentFps || src?.fps || 60;   // the game's own rate
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
