/* PlantRail front end - plain JavaScript, no build step.
   Talks only to the Python server (/api/...), which relays to the ESP32 controller and camera. */
(() => {
  'use strict';

  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const icon = (id) => `<svg class="ic"><use href="#i-${id}"/></svg>`;
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  const NS = 'http://www.w3.org/2000/svg';
  const POT_COLORS = ['#3f9a48', '#d9672c', '#2f7fe0'];
  const RING_LEN = 2 * Math.PI * 52;
  const TABS = ['home', 'diagnosis', 'care', 'history'];

  const S = {
    status: null,        // last /api/status reply
    scans: [],
    tab: 'home',
    range: 24,
    live: false,
    camOk: null,
    scanning: false,
    dirty: false,        // unsaved edits in the settings form
    prevState: null,
    potCount: 3,
  };

  /* ------------------------------------------------------------------ helpers */
  async function api(path, { method = 'GET', body, form } = {}) {
    const opt = { method, headers: {} };
    if (form) opt.body = form;
    else if (body !== undefined) { opt.headers['Content-Type'] = 'application/json'; opt.body = JSON.stringify(body); }
    let res;
    try { res = await fetch(path, opt); } catch { throw new Error('Cannot reach the PlantRail server'); }
    if (!res.ok) {
      let msg = `HTTP ${res.status}`;
      try { const j = await res.json(); msg = j.detail || j.error || msg; } catch { /* keep default */ }
      throw new Error(typeof msg === 'string' ? msg : JSON.stringify(msg));
    }
    return res.json();
  }

  function toast(msg, kind = '') {
    const t = document.createElement('div');
    t.className = 'toast' + (kind === 'err' ? ' err' : '');
    t.textContent = msg;
    $('#toasts').appendChild(t);
    setTimeout(() => t.remove(), kind === 'err' ? 5500 : 3200);
  }

  function ago(sec) {
    if (sec == null || sec < 0) return 'never';
    if (sec < 90) return 'just now';
    if (sec < 3600) return `${Math.round(sec / 60)} min ago`;
    if (sec < 86400) return `${Math.round(sec / 3600)} h ago`;
    return `${Math.round(sec / 86400)} d ago`;
  }
  const agoTs = (ts) => ago(Math.floor(Date.now() / 1000) - ts);
  const hhmm = (ts) => new Date(ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });

  /* ------------------------------------------------------------------ tabs */
  function showTab(name) {
    const t = TABS.includes(name) ? name : 'home';
    S.tab = t;
    $$('.view').forEach((v) => v.classList.toggle('on', v.id === 'view-' + t));
    $$('[data-tab]').forEach((a) => a.classList.toggle('on', a.dataset.tab === t));
    $$('.app-nav button').forEach((b) => b.classList.toggle('on', b.dataset.go === t));
    window.scrollTo({ top: 0 });
    if (t === 'care') loadScanProfile();
    if (t === 'history') loadHistory();
    if (t === 'diagnosis' || t === 'home') camTick(true);
  }
  window.addEventListener('hashchange', () => showTab(location.hash.slice(1)));

  document.addEventListener('click', (e) => {
    const go = e.target.closest('[data-go]');
    if (go) {
      e.preventDefault();
      if (location.hash.slice(1) === go.dataset.go) showTab(go.dataset.go);
      else location.hash = go.dataset.go;
    }
  });

  /* ------------------------------------------------------------------ commands to the controller */
  async function act(action, params = {}, btn) {
    if (btn) btn.disabled = true;
    try {
      const r = await api('/api/device/' + action, { method: 'POST', body: params });
      toast(r.msg || 'Done');
      await refreshStatus();
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      if (btn) btn.disabled = !(S.status && S.status.online);
    }
  }

  document.addEventListener('click', (e) => {
    const b = e.target.closest('[data-act]');
    if (!b || b.disabled) return;
    if (b.dataset.confirm && !window.confirm(b.dataset.confirm)) return;
    act(b.dataset.act, b.dataset.params ? JSON.parse(b.dataset.params) : {}, b);
  });

  /* ------------------------------------------------------------------ camera */
  let camTimer = 0;
  let camUrl = '';
  function setCam(ok) {
    S.camOk = ok;
    $$('.viewfinder .vf-empty').forEach((el) => { el.hidden = ok; });
    const pill = $('#pillCam');
    pill.className = 'pill ' + (ok ? 'ok' : 'bad');
    $('span', pill).textContent = ok ? 'Camera' : 'Camera offline';
  }

  let camId = 0;   // id of the newest frame we show; the server answers as soon as a newer one exists
  async function camTick() {
    clearTimeout(camTimer);
    const wanted = !document.hidden && (S.tab === 'home' || S.tab === 'diagnosis') && !(S.live && S.tab === 'diagnosis');
    if (!wanted) { camTimer = setTimeout(camTick, 1500); return; }
    try {
      const r = await fetch('/api/camera/snapshot?after=' + camId, { cache: 'no-store' });
      if (!r.ok) throw new Error('camera');
      const id = Number(r.headers.get('X-Frame-Id')) || 0;
      const isNew = id !== camId;
      if (isNew) {
        const url = URL.createObjectURL(await r.blob());
        $$('img.live').forEach((img) => { img.src = url; });
        if (camUrl) URL.revokeObjectURL(camUrl);
        camUrl = url;
        camId = id;
      }
      setCam(true);
      camTimer = setTimeout(camTick, isNew ? 150 : 800);
    } catch {
      setCam(false);
      camTimer = setTimeout(camTick, 5000);
    }
  }

  $('#liveToggle').addEventListener('change', (e) => {
    const big = $('.viewfinder.big img.live');
    const url = S.status && S.status.cam && S.status.cam.stream_url;
    if (e.target.checked) {
      if (!url) { toast('Live stream is not available (simulator or no CAM_STREAM_URL)', 'err'); e.target.checked = false; return; }
      S.live = true;
      big.onerror = () => { toast('Could not open the live stream', 'err'); e.target.checked = false; S.live = false; camTick(true); };
      big.src = url;
    } else {
      S.live = false;
      big.onerror = null;
      camTick(true);
    }
  });

  /* ------------------------------------------------------------------ AI diagnosis */
  const HEALTH = { healthy: ['ok', 'Healthy'], stressed: ['warn', 'Stressed'], diseased: ['bad', 'Diseased'], unknown: ['info', 'Unknown'] };
  const healthChip = (h) => HEALTH[h] || ['info', h || 'Unknown'];

  const URGENCY = { low: ['ok', 'Low urgency'], medium: ['warn', 'Act this week'], high: ['bad', 'Act today'] };

  /* ---- answer language (Bangla / English), remembered in this browser ---- */
  const langSel = $('#langSel');
  try { langSel.value = localStorage.getItem('plantrail.lang') || 'bn'; } catch { /* storage may be blocked */ }
  langSel.addEventListener('change', () => { try { localStorage.setItem('plantrail.lang', langSel.value); } catch { /* ignore */ } });
  const lang = () => langSel.value;

  /* ---- voice input (Chrome / Edge speech recognition, works on http://localhost) ---- */
  const SR = window.SpeechRecognition || window.webkitSpeechRecognition;
  function setupMic(btn, box) {
    if (!SR) { btn.disabled = true; btn.title = 'Voice input needs Chrome or Edge'; return; }
    let rec = null;
    btn.addEventListener('click', () => {
      if (rec) { rec.stop(); return; }
      if (!window.isSecureContext) { toast('Voice input works on http://localhost:8000 (on the PC), not on a phone over Wi-Fi', 'err'); return; }
      rec = new SR();
      rec.lang = lang() === 'bn' ? 'bn-BD' : 'en-US';
      rec.interimResults = true;
      rec.continuous = false;
      const base = box.value.trim() ? box.value.trim() + ' ' : '';
      btn.classList.add('listening');
      rec.onresult = (ev) => { let text = ''; for (const r of ev.results) text += r[0].transcript; box.value = base + text; };
      rec.onerror = (ev) => toast(ev.error === 'not-allowed' ? 'Allow the microphone in the browser address bar' : 'Voice input stopped: ' + ev.error, 'err');
      rec.onend = () => { btn.classList.remove('listening'); rec = null; };
      try { rec.start(); } catch { rec = null; btn.classList.remove('listening'); }
    });
  }
  setupMic($('#micNote'), $('#noteText'));
  setupMic($('#micAsk'), $('#askText'));

  /* ---- read aloud ---- */
  if ('speechSynthesis' in window) window.speechSynthesis.getVoices();  // primes the voice list
  function speak(text) {
    if (!('speechSynthesis' in window)) { toast('This browser cannot read aloud', 'err'); return; }
    const synth = window.speechSynthesis;
    synth.cancel();
    const want = lang();
    const voice = synth.getVoices().find((v) => v.lang.toLowerCase().startsWith(want));
    if (!voice && want === 'bn') toast('No Bangla voice found on this computer (Microsoft Edge usually has one). Using the default voice.', 'err');
    const u = new SpeechSynthesisUtterance(text);
    if (voice) u.voice = voice;
    u.lang = voice ? voice.lang : (want === 'bn' ? 'bn-BD' : 'en-US');
    synth.speak(u);
  }
  const speechText = (scan) => {
    const r = scan.result || {};
    const parts = [r.plant_common_name, r.summary];
    (r.issues || []).forEach((i) => { parts.push(i.name, i.symptoms_seen, i.likely_cause, ...(i.treatment || [])); });
    parts.push(...(r.prevention || []), r.watering_advice);
    return parts.filter(Boolean).join('. ');
  };
  document.addEventListener('click', (e) => {
    const b = e.target.closest('[data-speak]');
    if (b && S.currentScan) speak(speechText(S.currentScan));
  });

  /* ---- scanning ---- */
  function setScanBusy(b, text) {
    S.scanning = b;
    const vf = $('.viewfinder.big');
    vf.classList.toggle('busy', b);
    $('.vf-busy', vf).hidden = !b;
    if (text) $('#busyText').textContent = text;
    ['#scanBtn', '#sweepScanBtn', '#uploadBtn'].forEach((id) => { $(id).disabled = b; });
    $('#scanProgress').hidden = !b || !S.sweeping;
  }

  async function pollProgress() {
    try {
      const p = await api('/api/scan/progress');
      if (!p.running) return;
      $('#progressText').textContent = p.text;
      $('#busyText').textContent = p.text;
      $('#progressBar').style.width = (p.total ? (100 * p.step) / p.total : 0) + '%';
    } catch { /* the next tick will try again */ }
  }

  function scanForm(file) {
    const fd = new FormData();
    const pot = $('#scanPot').value;
    if (pot) fd.append('pot', pot);
    const note = $('#noteText').value.trim();
    if (note) fd.append('note', note);
    fd.append('lang', lang());
    if (file) fd.append('file', file);
    return fd;
  }

  async function runScan(kind, file) {
    if (S.scanning) return;
    S.sweeping = kind === 'sweep';
    setScanBusy(true, S.sweeping ? 'Turning the plant and taking photos…' : 'Analysing…');
    const timer = S.sweeping ? setInterval(pollProgress, 1000) : 0;
    try {
      const scan = await api(S.sweeping ? '/api/scan/sweep' : '/api/diagnose', { method: 'POST', form: scanForm(file) });
      S.scans.unshift(scan);
      renderResult(scan);
      renderScans();
      renderPhone();
      toast('Scan complete');
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      if (timer) clearInterval(timer);
      S.sweeping = false;
      setScanBusy(false);
    }
  }
  $('#sweepScanBtn').addEventListener('click', () => runScan('sweep'));
  $('#scanBtn').addEventListener('click', () => runScan('one'));
  $('#uploadBtn').addEventListener('click', () => $('#uploadInput').click());
  $('#uploadInput').addEventListener('change', (e) => {
    const f = e.target.files[0];
    e.target.value = '';
    if (f) runScan('one', f);
  });

  /* ---- follow-up questions about the result ---- */
  function addBubble(kind, text, speakable) {
    const log = $('#askLog');
    const b = document.createElement('div');
    b.className = 'bubble ' + kind;
    b.textContent = text;
    if (speakable) {
      const s = document.createElement('button');
      s.className = 'icon-btn';
      s.type = 'button';
      s.setAttribute('aria-label', 'Read aloud');
      s.innerHTML = icon('speaker');
      s.addEventListener('click', () => speak(text));
      b.append(s);
    }
    log.append(b);
    b.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  }

  async function askQuestion() {
    const box = $('#askText');
    const q = box.value.trim();
    if (!q) return;
    box.value = '';
    addBubble('q', q);
    const btn = $('#askBtn');
    btn.disabled = true;
    try {
      const r = await api('/api/ask', { method: 'POST', body: { question: q, scan_id: S.currentScan ? S.currentScan.id : null, lang: lang() } });
      addBubble('a', r.answer, true);
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      btn.disabled = false;
    }
  }
  $('#askBtn').addEventListener('click', askQuestion);
  $('#askText').addEventListener('keydown', (e) => { if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); askQuestion(); } });

  function renderResult(scan) {
    const r = scan.result || {};
    const [cls, label] = healthChip(r.health_status);
    const conf = Math.round(clamp(r.identification_confidence || 0, 0, 1) * 100);
    const issues = (r.issues || []).map((i) => `
      <div class="issue">
        <div class="issue-head"><h4>${esc(i.name)}</h4><em class="chip ${i.confidence >= 0.6 ? 'bad' : 'warn'}">${esc(i.category)} · ${Math.round((i.confidence || 0) * 100)}%</em></div>
        <p><b>Seen:</b> ${esc(i.symptoms_seen)}</p>
        <p><b>Likely cause:</b> ${esc(i.likely_cause)}</p>
        ${(i.treatment || []).length ? `<ol>${i.treatment.map((t) => `<li>${esc(t)}</li>`).join('')}</ol>` : ''}
      </div>`).join('');
    const tips = (r.care_tips || []).map((t) => `<li>${icon('sprout')}<span>${esc(t)}</span></li>`).join('');
    const prevent = (r.prevention || []).map((t) => `<li>${icon('shield')}<span>${esc(t)}</span></li>`).join('');
    const meta = [`Identification ${conf}%`, `photo ${esc(r.image_quality || 'n/a')}`, scan.pot ? `Pot ${scan.pot}` : null, hhmm(scan.ts)].filter(Boolean).join(' · ');
    const urgency = r.health_status !== 'healthy' && URGENCY[r.urgency] ? `<em class="chip ${URGENCY[r.urgency][0]}">${URGENCY[r.urgency][1]}</em>` : '';
    const urls = scan.image_urls && scan.image_urls.length ? scan.image_urls : [scan.image_url];
    const photos = urls.length > 1
      ? `<div class="photo-strip">${urls.map((u, i) => `<figure><img src="${esc(u)}" alt="" loading="lazy"><figcaption>${esc((scan.labels || [])[i] || '')}</figcaption></figure>`).join('')}</div>`
      : `<img class="res-photo" src="${esc(urls[0])}" alt="Scanned photo">`;
    S.currentScan = scan;
    $('#resultBody').innerHTML = `
      ${scan.demo ? '<div class="demo-note">Demo result. Add a free GEMINI_API_KEY to the .env file for real AI diagnosis.</div>' : ''}
      <div class="res-head">
        <div><h3>${esc(r.plant_common_name || 'Unknown plant')}</h3><div class="latin">${esc(r.plant_scientific_name || '')}</div></div>
        <div class="chips"><em class="chip ${cls}">${label}</em>${urgency}</div>
      </div>
      <div class="conf"><i style="width:${conf}%"></i></div>
      <div class="res-meta">${meta}</div>
      ${photos}
      ${scan.note ? `<blockquote class="user-note">${esc(scan.note)}</blockquote>` : ''}
      ${r.needs_retake ? '<div class="retake-note">The photo is not clear enough. Please retake it with better light and focus.</div>' : ''}
      <p class="res-summary">${esc(r.summary)}</p>
      <button class="btn ghost sm" type="button" data-speak>${icon('speaker')}Read aloud</button>
      ${issues ? `<div class="block-title">Disease details and treatment</div>${issues}` : ''}
      ${prevent ? `<div class="block-title">How to prevent it</div><ul class="tips">${prevent}</ul>` : ''}
      ${tips ? `<div class="block-title">Care tips</div><ul class="tips">${tips}</ul>` : ''}
      ${r.watering_advice ? `<div class="water-advice">${icon('can')}<span>${esc(r.watering_advice)}</span></div>` : ''}`;
    $('#askBox').hidden = false;
    $('#askLog').replaceChildren();
  }

  function renderScans() {
    const card = (s) => {
      const [cls, label] = healthChip(s.result.health_status);
      return `<button class="shot" data-scan="${s.id}">
        <img src="${esc(s.image_url)}" alt="" loading="lazy">
        <div><b>${esc(s.result.plant_common_name || 'Unknown plant')}</b><span><em class="chip ${cls}">${label}</em></span>
        <small>${agoTs(s.ts)}${s.pot ? ' · Pot ' + s.pot : ''}${s.source === 'upload' ? ' · upload' : ''}${s.demo ? ' · demo' : ''}</small></div></button>`;
    };
    const none = '<p class="none">No scans yet.</p>';
    $('#gallery').innerHTML = S.scans.length ? S.scans.slice(0, 6).map(card).join('') : none;
    $('#gallery2').innerHTML = S.scans.length ? S.scans.map(card).join('') : none;
  }
  document.addEventListener('click', (e) => {
    const b = e.target.closest('[data-scan]');
    if (!b) return;
    const scan = S.scans.find((s) => String(s.id) === b.dataset.scan);
    if (scan) { renderResult(scan); if (S.tab !== 'diagnosis') location.hash = 'diagnosis'; $('#resultPanel').scrollIntoView({ block: 'start', behavior: 'smooth' }); }
  });

  function computeTip() {
    const d = S.status && S.status.device;
    if (d) {
      const dry = (d.pots || []).find((p) => p.enabled && p.sensor_ok && p.moisture <= p.threshold);
      if (dry) return `Pot ${dry.id} is at ${dry.moisture}%. It will be watered at the next check, or tap Water now.`;
    }
    const last = S.scans[0];
    const tip = last && last.result && (last.result.care_tips || [])[0];
    return tip || 'Water when the top inch of soil is dry.';
  }

  function renderPhone() {
    const s = S.scans[0];
    const card = $('#phoneResult');
    if (s) {
      const [cls, label] = healthChip(s.result.health_status);
      card.innerHTML = `<div class="thumb"><img src="${esc(s.image_url)}" alt=""></div>
        <div><b>${esc(s.result.plant_common_name || 'Unknown plant')}</b><span>${esc(s.result.plant_scientific_name || '')}</span></div>
        <em class="chip ${cls}">${label}</em>`;
    }
    $('#phoneTip').textContent = computeTip();
  }

  /* ------------------------------------------------------------------ soil rings, tiles */
  function buildRings() {
    $('#rings').innerHTML = [1, 2, 3].map((n) => `
      <article class="ring-card" data-pot="${n}">
        <div class="ring"><svg viewBox="0 0 120 120"><circle class="bg" cx="60" cy="60" r="52"/><circle class="fg" cx="60" cy="60" r="52" stroke-dasharray="${RING_LEN}" stroke-dashoffset="${RING_LEN}"/></svg>
          <div class="val"><b><span class="pct">--</span><small>%</small></b><i>moisture</i></div></div>
        <div class="ring-info">
          <h4>Pot ${n} <em class="chip st">-</em></h4>
          <ul><li class="l-last">Last watered: -</li><li class="l-thr">Waters below -</li><li class="l-cnt"></li></ul>
          <button class="btn ghost" data-act="water" data-params='{"pot":${n}}'>${icon('drop')}Water now</button>
        </div>
      </article>`).join('');
  }

  const potStatus = (p) => {
    if (!p.sensor_ok) return ['off', 'No sensor', ''];
    if (p.moisture <= p.threshold) return ['dry', 'Needs water', 'warn'];
    if (p.moisture >= 80) return ['wet', 'Saturated', 'info'];
    return ['ok', 'Healthy', 'ok'];
  };

  function updateRings(d) {
    (d ? d.pots : []).forEach((p) => {
      const card = $(`.ring-card[data-pot="${p.id}"]`);
      if (!card) return;
      const [state, label, chip] = potStatus(p);
      const ring = $('.ring', card);
      ring.className = 'ring ' + (state === 'ok' ? '' : state);
      $('.fg', card).style.strokeDashoffset = RING_LEN * (1 - clamp(p.moisture, 0, 100) / 100);
      $('.pct', card).textContent = p.sensor_ok ? p.moisture : '--';
      const st = $('.st', card);
      st.textContent = label;
      st.className = 'chip st ' + chip;
      $('.l-last', card).textContent = 'Last watered: ' + ago(p.last_water_s);
      $('.l-thr', card).textContent = `Waters below ${p.threshold}%${p.enabled ? '' : ' (disabled)'}`;
      $('.l-cnt', card).textContent = p.count_today ? `${p.count_today} watering${p.count_today > 1 ? 's' : ''} in 24 h` : '';
      card.classList.toggle('busy', d.active_pot === p.id && (d.state === 'watering'));
    });
  }

  function updateTiles(s) {
    const d = s.device;
    if (!d) { $('#tiles').innerHTML = '<div class="tile bad"><span>Controller</span><b>Offline</b><small>Check power, Wi-Fi and ESP32_URL</small></div>'; return; }
    const pct = d.rail_steps > 0 ? Math.round((d.pos / d.rail_steps) * 100) : null;
    const rssiQ = d.rssi > -60 ? 'Strong' : d.rssi > -75 ? 'Good' : 'Weak';
    const next = d.auto && d.calibrated && d.next_check_s >= 0 ? `Next check in ${Math.max(1, Math.round(d.next_check_s / 60))} min` : '';
    const tiles = [
      ['Controller', d.state[0].toUpperCase() + d.state.slice(1), d.message, d.state === 'error' ? 'bad' : d.state === 'idle' ? 'ok' : 'warn'],
      ['Carriage', pct == null ? '-' : pct + '%', `${d.pos} steps${d.homed ? '' : ' · not homed'}`, d.homed ? '' : 'warn'],
      ['Ultrasonic', d.distance_cm > 0 ? d.distance_cm.toFixed(1) + ' cm' : '-', d.baseline_cm > 0 ? `empty base ${d.baseline_cm.toFixed(1)} cm` : 'not calibrated', ''],
      ['Pump', d.pump ? 'ON' : 'Off', d.pump ? 'Watering' : 'Idle', d.pump ? 'warn' : ''],
      ['IR sensors', d.ir_home ? 'At Home' : d.ir_end ? 'At End' : 'Between', `Home ${d.ir_home ? '●' : '○'}  End ${d.ir_end ? '●' : '○'}`, ''],
      ['Auto watering', d.auto ? 'On' : 'Off', next || (d.calibrated ? '' : 'calibrate first'), d.auto ? 'ok' : ''],
      ['Wi-Fi', rssiQ, `${d.rssi} dBm · ${d.ip}`, ''],
    ];
    $('#tiles').innerHTML = tiles.map(([k, v, sub, c]) => `<div class="tile ${c}"><span>${k}</span><b>${esc(v)}</b><small>${esc(sub)}</small></div>`).join('');
    $('#nextCheck').textContent = next;
  }

  /* ------------------------------------------------------------------ rail drawing */
  function buildRail() {
    const g = $('#pots');
    g.innerHTML = [1, 2, 3].map((n) => `
      <g class="pot-g" data-pot="${n}" transform="translate(0 0)">
        <rect class="pot-lid" x="-34" y="266" width="68" height="9" rx="4.5"/>
        <path class="pot-body" d="M-36 286H36L28 336H-28Z"/>
        <ellipse class="pot-soil" cx="0" cy="286" rx="36" ry="7"/>
        <text class="pot-num" x="0" y="322" text-anchor="middle">${n}</text>
        <text class="pot-label" x="0" y="360" text-anchor="middle">Pot ${n}</text>
        <rect class="pot-track" x="-32" y="368" width="64" height="7" rx="3.5"/>
        <rect class="pot-fill" x="-32" y="368" width="0" height="7" rx="3.5"/>
      </g>`).join('');
  }

  function updateRail(d) {
    if (!d) return;
    const rail = d.rail_steps;
    const xOf = (steps) => 100 + (rail > 0 ? clamp(steps / rail, 0, 1) : 0) * 520;
    (d.pots || []).forEach((p, i) => {
      const g = $(`.pot-g[data-pot="${p.id}"]`);
      const x = d.calibrated ? xOf(p.pos) : [170, 360, 550][i];
      g.setAttribute('transform', `translate(${x} 0)`);
      const [state] = potStatus(p);
      g.setAttribute('class', `pot-g ${state === 'dry' ? 'dry' : state === 'wet' ? 'wet' : ''} ${d.active_pot === p.id && d.state !== 'idle' ? 'active' : ''}`);
      $('.pot-fill', g).setAttribute('width', 64 * clamp(p.moisture, 0, 100) / 100);
      $('.pot-label', g).textContent = p.sensor_ok ? `Pot ${p.id} · ${p.moisture}%` : `Pot ${p.id}`;
    });
    const car = $('#carriage');
    car.setAttribute('transform', `translate(${xOf(d.pos)} 0)`);
    car.classList.toggle('faded', !d.homed);
    car.classList.toggle('pumping', !!d.pump);
    $('#irHome .ir').classList.toggle('on', d.ir_home);
    $('#irEnd .ir').classList.toggle('on', d.ir_end);
    $('#coneVal').textContent = d.distance_cm > 0 ? d.distance_cm.toFixed(1) + ' cm' : (d.state === 'idle' ? '-' : 'scanning');

    const chip = $('#railState');
    chip.textContent = d.state;
    chip.className = 'state-chip ' + (d.state === 'error' ? 'error' : d.state === 'idle' ? '' : 'busy');
    $('#railMsg').textContent = d.state === 'error' ? d.error : d.message + (d.queue > 1 ? ` · ${d.queue - 1} more queued` : '');
    const auto = $('#autoToggle');
    if (document.activeElement !== auto) auto.checked = !!d.auto;
    const tt = d.turntable;
    $('#turnDeg').textContent = tt ? (tt.sweeping ? ` Sweeping… now at ${tt.deg}°.` : ` Now at ${tt.deg}° from its start position.`) : '';
    $('#teachPos').textContent = d.pos;
    $('#teachSensors').textContent = `· IR 1 ${d.ir_home ? '●' : '○'}  IR 2 ${d.ir_end ? '●' : '○'}${d.homed ? '' : ' · home not set'}`;
    $('#sweepBtn').dataset.busy = tt && tt.busy ? '1' : '';
    $('#sweepBtn').disabled = !!(tt && tt.busy) || !S.status.online;
  }

  /* ------------------------------------------------------------------ settings form */
  function buildPotTable() {
    $('#potTable').innerHTML = [1, 2, 3].map((n) => `
      <div class="pot-row" data-pot="${n}">
        <div class="pot-row-head">
          <b>Pot ${n}</b>
          <label class="switch"><input type="checkbox" data-setting="e${n}"><span class="knob"></span>Enabled</label>
        </div>
        <div class="pot-row-grid">
          <label class="slider-field">Water when moisture is below <output>35%</output>
            <input type="range" min="5" max="90" step="1" data-setting="t${n}"></label>
          <label class="field">Pump time <span class="unit">seconds</span><input type="number" min="1" max="30" data-setting="s${n}"></label>
        </div>
        <div class="pot-actions">
          <button class="btn ghost sm" data-act="goto" data-params='{"pot":${n}}'>Go to pot</button>
          <button class="btn ghost sm" data-act="water" data-params='{"pot":${n}}'>${icon('drop')}Water now</button>
          <button class="btn ghost sm" data-act="soilcal" data-params='{"pot":${n},"point":"dry"}' title="Hold the sensor in the AIR first">Set dry <span class="raw-dry"></span></button>
          <button class="btn ghost sm" data-act="soilcal" data-params='{"pot":${n},"point":"wet"}' title="Put the sensor in WATER first">Set wet <span class="raw-wet"></span></button>
          <span class="hint" style="margin:0;align-self:center">raw <b class="raw-now">-</b> · distance <b class="dist-now">-</b></span>
        </div>
      </div>`).join('');
  }

  function fillSettings(d) {
    if (!d) return;
    $$('.pot-row').forEach((row) => {
      const p = d.pots[row.dataset.pot - 1];
      $('.raw-now', row).textContent = p.raw;
      $('.dist-now', row).textContent = p.dist_cm > 0 ? p.dist_cm.toFixed(1) + ' cm' : '-';
      $('.raw-dry', row).textContent = `(${p.dry})`;
      $('.raw-wet', row).textContent = `(${p.wet})`;
    });
    if (S.dirty) return;
    const set = (key, val) => {
      const el = $(`[data-setting="${key}"]`);
      if (!el || document.activeElement === el) return;
      if (el.type === 'checkbox') el.checked = !!val; else el.value = val;
      const out = el.closest('label') && $('output', el.closest('label'));
      if (out) out.textContent = val + '%';
    };
    const c = d.settings;
    set('check_min', c.check_min); set('soak_min', c.soak_min); set('nozzle_offset', c.nozzle_offset);
    set('delta_cm', c.delta_cm); set('pot_closer', c.pot_closer ? '1' : '0'); set('park', c.park ? '1' : '0'); set('verify', c.verify ? '1' : '0');
    set('pump_low', c.pump_mode !== undefined ? c.pump_mode : (c.pump_active_low ? 1 : 0)); set('rail_speed', c.rail_speed); set('scan_speed', c.scan_speed);
    d.pots.forEach((p) => { set('e' + p.id, p.enabled); set('t' + p.id, p.threshold); set('s' + p.id, p.water_secs); });
  }

  document.addEventListener('input', (e) => {
    const el = e.target.closest('[data-setting]');
    if (!el) return;
    S.dirty = true;
    const out = el.closest('label') && $('output', el.closest('label'));
    if (out) out.textContent = el.value + '%';
  });

  $('#saveSettings').addEventListener('click', async (e) => {
    const params = {};
    $$('[data-setting]').forEach((el) => { params[el.dataset.setting] = el.type === 'checkbox' ? (el.checked ? 1 : 0) : el.value; });
    await act('settings', params, e.currentTarget);
    S.dirty = false;
  });

  $('#autoToggle').addEventListener('change', (e) => act('auto', { on: e.target.checked ? 1 : 0 }));

  /* ------------------------------------------------------------------ status polling */
  function renderStatus() {
    const s = S.status;
    const d = s.device;
    const ctl = $('#pillCtl');
    ctl.className = 'pill ' + (s.online ? 'ok' : 'bad');
    $('span', ctl).textContent = s.online ? 'Controller' : 'Controller offline';
    const ai = $('#pillAi');
    ai.className = 'pill ' + (s.ai.demo ? 'warn' : 'ok');
    $('span', ai).textContent = s.ai.demo ? 'AI demo' : 'AI ready';
    $('#aiHint').textContent = s.ai.demo
      ? 'AI is in demo mode: add a free GEMINI_API_KEY to the .env file for real diagnoses.'
      : `Diagnosis by ${s.ai.provider === 'gemini' ? 'Google Gemini' : 'Claude'} (${s.ai.model}).${s.auto_scan_hours ? ` Automatic scan every ${s.auto_scan_hours} h.` : ''}`;
    $('#footMode').textContent = s.mock ? 'Simulator mode' : (d ? `Controller ${d.ip}` : 'Controller offline');

    const bar = $('#alertBar');
    let alert = null;
    if (!s.online) alert = ['', 'The controller is offline. Check its power, the Wi-Fi and ESP32_URL in the .env file.'];
    else if (d.state === 'error') alert = ['', 'Controller error: ' + d.error];
    else if (!d.calibrated) alert = ['warn', 'The rail is not set up yet. Open Care and use “Manual setup” (or “Calibrate rail”) so the carriage knows where the pots are.'];
    bar.hidden = !alert;
    if (alert) { bar.className = 'banner-alert ' + alert[0]; bar.textContent = alert[1]; }

    $$('[data-act]').forEach((b) => { if (!b.dataset.busy) b.disabled = !s.online; });
    updateRings(d);
    updateTiles(s);
    updateRail(d);
    fillSettings(d);
    renderPhone();

    if (d) {
      if (S.prevState === 'calibrating' && d.state !== 'calibrating') loadScanProfile();
      S.prevState = d.state;
    }
  }

  async function refreshStatus() {
    try {
      S.status = await api('/api/status');
      renderStatus();
    } catch {
      const ctl = $('#pillCtl');
      ctl.className = 'pill bad';
      $('span', ctl).textContent = 'Server offline';
    }
  }

  /* ------------------------------------------------------------------ charts */
  function svgEl(name, attrs = {}, text) {
    const el = document.createElementNS(NS, name);
    Object.entries(attrs).forEach(([k, v]) => el.setAttribute(k, v));
    if (text != null) el.textContent = text;
    return el;
  }

  function drawScan(data) {
    const box = $('#scanChart');
    const dist = (data && data.dist) || [];
    const valid = dist.filter((v) => v > 0).map((v) => v / 10);
    if (dist.length < 5 || !valid.length) {
      box.innerHTML = '<div class="none">No scan data yet. Press “Calibrate rail” to scan.</div>';
      return;
    }
    const W = 720, H = 260, L = 46, R = 12, T = 14, B = 32;
    const lo = Math.floor(Math.min(...valid) - 2), hi = Math.ceil(Math.max(...valid) + 2);
    const x = (i) => L + (i / (dist.length - 1)) * (W - L - R);
    const y = (cm) => T + (1 - (cm - lo) / (hi - lo)) * (H - T - B);
    const svg = svgEl('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': 'Ultrasonic scan along the rail' });

    for (let k = 0; k <= 4; k++) {
      const v = lo + ((hi - lo) * k) / 4;
      svg.append(svgEl('line', { x1: L, x2: W - R, y1: y(v), y2: y(v), stroke: '#e6ece3' }));
      svg.append(svgEl('text', { x: L - 8, y: y(v) + 4, 'text-anchor': 'end', 'font-size': 11, fill: '#667a6a' }, v.toFixed(0) + ' cm'));
    }
    const base = data.baseline_cm, thr = data.threshold_cm;
    if (base > 0) {
      svg.append(svgEl('rect', { x: L, width: W - L - R, y: y(Math.min(hi, base + thr)), height: Math.max(0, y(Math.max(lo, base - thr)) - y(Math.min(hi, base + thr))), fill: 'rgba(60,90,60,.07)' }));
      svg.append(svgEl('line', { x1: L, x2: W - R, y1: y(base), y2: y(base), stroke: '#6a8f5b', 'stroke-dasharray': '5 4' }));
      svg.append(svgEl('text', { x: W - R, y: y(base) - 5, 'text-anchor': 'end', 'font-size': 11, fill: '#6a8f5b' }, `empty base ${base.toFixed(1)} cm`));
    }
    (data.pots || []).forEach((p, i) => {
      svg.append(svgEl('rect', { x: x(p.s), width: Math.max(3, x(p.e) - x(p.s)), y: T, height: H - T - B, fill: 'rgba(79,154,74,.18)', rx: 4 }));
      svg.append(svgEl('text', { x: (x(p.s) + x(p.e)) / 2, y: T + 14, 'text-anchor': 'middle', 'font-size': 12, 'font-weight': 700, fill: POT_COLORS[i % 3] }, 'Pot ' + (i + 1)));
    });
    let dPath = '', pen = false;
    dist.forEach((v, i) => {
      if (v <= 0) { pen = false; return; }
      dPath += `${pen ? 'L' : 'M'}${x(i).toFixed(1)} ${y(v / 10).toFixed(1)}`;
      pen = true;
    });
    svg.append(svgEl('path', { d: dPath, fill: 'none', stroke: '#2f6b3a', 'stroke-width': 2, 'stroke-linejoin': 'round' }));
    [0, 0.5, 1].forEach((f) => {
      svg.append(svgEl('text', { x: L + f * (W - L - R), y: H - 8, 'text-anchor': f === 0 ? 'start' : f === 1 ? 'end' : 'middle', 'font-size': 11, fill: '#667a6a' }, Math.round(f * dist.length * data.step) + ' steps'));
    });
    box.replaceChildren(svg);
  }

  async function loadScanProfile() {
    try { drawScan(await api('/api/device/scan')); }
    catch { $('#scanChart').innerHTML = '<div class="none">Scan data is not available (controller offline).</div>'; }
  }
  $('#reloadScan').addEventListener('click', loadScanProfile);

  function drawMoisture(points) {
    const box = $('#moistChart');
    if (points.length < 2) {
      box.innerHTML = '<div class="none">Collecting data. The server stores one reading per minute while the controller is online.</div>';
      return;
    }
    const W = 720, H = 270, L = 40, R = 12, T = 12, B = 30;
    const t1 = Math.floor(Date.now() / 1000), t0 = t1 - S.range * 3600;
    const x = (t) => L + ((t - t0) / (t1 - t0)) * (W - L - R);
    const y = (v) => T + (1 - v / 100) * (H - T - B);
    const svg = svgEl('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': 'Soil moisture over time' });
    [0, 25, 50, 75, 100].forEach((v) => {
      svg.append(svgEl('line', { x1: L, x2: W - R, y1: y(v), y2: y(v), stroke: '#e6ece3' }));
      svg.append(svgEl('text', { x: L - 8, y: y(v) + 4, 'text-anchor': 'end', 'font-size': 11, fill: '#667a6a' }, v + '%'));
    });
    for (let k = 0; k <= 4; k++) {
      const t = t0 + ((t1 - t0) * k) / 4;
      const dt = new Date(t * 1000);
      const label = S.range > 24 ? dt.toLocaleDateString([], { day: 'numeric', month: 'short' }) : hhmm(t);
      svg.append(svgEl('text', { x: x(t), y: H - 8, 'text-anchor': k === 0 ? 'start' : k === 4 ? 'end' : 'middle', 'font-size': 11, fill: '#667a6a' }, label));
    }
    for (let i = 0; i < 3; i++) {
      let d = '', pen = false;
      points.forEach((p) => {
        if (p.p[i] == null) { pen = false; return; }
        d += `${pen ? 'L' : 'M'}${x(p.ts).toFixed(1)} ${y(p.p[i]).toFixed(1)}`;
        pen = true;
      });
      svg.append(svgEl('path', { d, fill: 'none', stroke: POT_COLORS[i], 'stroke-width': 2.5, 'stroke-linejoin': 'round', 'stroke-linecap': 'round' }));
    }
    const cursor = svgEl('line', { y1: T, y2: H - B, stroke: '#1c4a2a', 'stroke-width': 1, opacity: 0 });
    svg.append(cursor);
    const tip = document.createElement('div');
    tip.className = 'chart-tip';
    tip.hidden = true;
    box.replaceChildren(svg, tip);
    svg.addEventListener('pointermove', (ev) => {
      const r = svg.getBoundingClientRect();
      const t = t0 + (((ev.clientX - r.left) / r.width) * W - L) / (W - L - R) * (t1 - t0);
      const p = points.reduce((a, b) => (Math.abs(b.ts - t) < Math.abs(a.ts - t) ? b : a));
      cursor.setAttribute('x1', x(p.ts)); cursor.setAttribute('x2', x(p.ts)); cursor.setAttribute('opacity', 1);
      tip.hidden = false;
      tip.style.left = (x(p.ts) / W) * r.width + 'px';
      tip.style.top = '30px';
      tip.textContent = `${hhmm(p.ts)} · ${p.p.map((v, i) => `P${i + 1} ${v == null ? '-' : v + '%'}`).join(' · ')}`;
    });
    svg.addEventListener('pointerleave', () => { cursor.setAttribute('opacity', 0); tip.hidden = true; });
  }

  const EVENT_VIEW = {
    water: ['drop', 'ok', (e) => `Watered pot ${e.pot}`, (e) => `${e.secs} s of water`],
    pump: ['drop', '', (e) => `Pump started, pot ${e.pot}`, (e) => `${e.secs} s · soil was at ${e.moisture}%`],
    auto: ['pulse', 'ok', () => 'Automatic check', (e) => e.msg],
    calibrated: ['ruler', 'ok', () => 'Rail calibrated', (e) => e.msg],
    error: ['warn', 'err', () => 'Error', (e) => e.msg],
    moved: ['warn', '', () => 'Carriage moved - going Home', (e) => e.msg],
    recover: ['ruler', '', () => 'Position recovered', (e) => e.msg],
  };

  async function loadHistory() {
    try {
      const [m, ev, sc] = await Promise.all([
        api(`/api/history/moisture?hours=${S.range}`),
        api('/api/history/events?limit=60'),
        api('/api/scans?limit=24'),
      ]);
      drawMoisture(m.points);
      $('#eventLog').innerHTML = ev.events.length
        ? ev.events.map((e) => {
          const [ic, cls, title, sub] = EVENT_VIEW[e.type] || ['pulse', '', () => e.type, (x) => x.msg];
          return `<li><span class="badge ${cls}">${icon(ic)}</span><div><b>${esc(title(e))}</b><small>${esc(sub(e))}</small></div><time title="${new Date(e.ts * 1000).toLocaleString()}">${agoTs(e.ts)}</time></li>`;
        }).join('')
        : '<li><span class="none">No activity yet.</span></li>';
      S.scans = sc.scans;
      renderScans();
      renderPhone();
    } catch (e) {
      toast(e.message, 'err');
    }
  }

  $('#rangeSeg').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-h]');
    if (!b) return;
    S.range = Number(b.dataset.h);
    $$('#rangeSeg button').forEach((x) => x.classList.toggle('on', x === b));
    loadHistory();
  });

  /* ------------------------------------------------------------------ start */
  buildRings();
  buildRail();
  buildPotTable();
  showTab(location.hash.slice(1));
  refreshStatus();
  setInterval(() => { if (!document.hidden) refreshStatus(); }, 2000);
  setInterval(() => { if (!document.hidden && S.tab === 'history') loadHistory(); }, 30000);
  api('/api/scans?limit=24').then((r) => { S.scans = r.scans; renderScans(); renderPhone(); if (r.scans[0]) renderResult(r.scans[0]); }).catch(() => {});
})();
