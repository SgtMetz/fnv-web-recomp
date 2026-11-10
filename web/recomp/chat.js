// Chat (web/recomp/index.html): a room where players talk to each other, under the game. A classic script; it
// connects only once the chat has been scrolled into view, and keeps no connection open for a player who never looks.
//
// The server is a WebSocket at the URL in <meta name="fnv-chat"> (empty: no chat, the panel says so). A deployment
// points it at its own room; the development page also takes ?chat=ws://... . Protocol, from the server:
// {type: 'hello', history, online, you: {country}}, {type: 'msg', id, name, country, text, t}, {type: 'online', n},
// {type: 'error', error}; to it: {type: 'msg', name, text}. The country (an ISO code, or '') is the server's, from
// where the connection comes from; flags come from <server>/flag/<cc>.svg.
//
// Names: Anonymous unless the player picks one in the name box over the messages; the box can be dismissed, and the
// name in the chat's header opens it again. Both are remembered on this browser (fnv.chat.name,
// fnv.chat.nameDismissed).
//
// The site owner's name is reserved on the server: picking it (or a lookalike) asks for a PIN, which the server checks
// ({type: 'auth', pin} -> {type: 'auth', ok}); the PIN is never in this page. Once it is accepted, this browser keeps it
// (fnv.chat.pin) to unlock the name again on the next connection. The owner's messages (owner: true) get a crown.
//
// Translation: a message goes out with its language only when the browser's LanguageDetector is sure of it (a
// browser set to English says nothing about what its player types); otherwise the reader's translator detects it.
// A tag that cannot be right (a Latin-script language on Cyrillic, CJK, ... text) is ignored. Each reader sees
// messages in their own language: the browser's language by default, or the one picked next to the Translate switch
// (fnv.chat.translate, fnv.chat.lang). Translations come from
// Chrome's on-device Translator API where it is ready, else the chat server ({type: 'translate'}), which translates
// each message once per language with Google's free web translator and shares it with every reader (so one page load
// does not send Google dozens of requests, which it throttles), else Google's translator straight from this browser.
// "show original" under a translated message swaps it back.
(() => {
  'use strict';
  const NAME_MAX = 24, TEXT_MAX = 300;

  const CSS = `
  #chat { margin: 14px 0 20px; border: 1px solid #3a3528; background: #14130f; border-radius: 6px; display: flex; flex-direction: column; }
  #chat.collapsed > :not(.head) { display: none; }
  #chat .head { display: flex; gap: 10px; align-items: baseline; padding: 8px 12px; border-bottom: 1px solid #2a271f; }
  #chat .head h2 { margin: 0; font-size: 14px; font-weight: 600; color: var(--accent, #e0a040); }
  #chat .head .online { color: var(--dim, #8a8478); }
  #chat .head .me { margin-left: auto; color: var(--dim, #8a8478); }
  #chat .head .me button { font: inherit; color: var(--fg, #d8d2c4); background: none; border: 0; padding: 0; cursor: pointer; text-decoration: underline dotted; }
  #chat .namebox { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; padding: 8px 12px; background: #1b1913; border-bottom: 1px solid #2a271f; }
  #chat .namebox[hidden] { display: none; }
  #chat .namebox label { color: var(--dim, #8a8478); }
  #chat .namebox .x { margin-left: auto; font-size: 16px; line-height: 1; padding: 2px 8px; }
  #chat input { font: inherit; color: var(--fg, #d8d2c4); background: #0d0d0d; border: 1px solid #3a3528; border-radius: 4px; padding: 6px 8px; }
  #chat input:focus { border-color: var(--accent, #e0a040); outline: none; }
  #chat button { font: inherit; color: var(--fg, #d8d2c4); background: #1e1c16; border: 1px solid #3a3528; border-radius: 4px; padding: 6px 12px; cursor: pointer; }
  #chat button:hover:not(:disabled) { border-color: var(--accent, #e0a040); }
  #chat button:disabled { opacity: .5; cursor: default; }
  #chat .msgs { height: 300px; overflow-y: auto; padding: 8px 12px; overscroll-behavior: contain; }
  #chat .msg { margin: 0 0 6px; overflow-wrap: anywhere; }
  #chat .msg .flag { display: inline-block; width: 20px; height: 15px; margin-right: 6px; vertical-align: -2px; border-radius: 2px;
    object-fit: cover; box-shadow: 0 0 0 1px rgba(255,255,255,.12); }
  #chat .msg .noflag { display: inline-block; width: 20px; margin-right: 6px; text-align: center; color: var(--dim, #8a8478); }
  #chat .head { flex-wrap: wrap; }
  #chat .head .tr { display: inline-flex; gap: 6px; align-items: center; color: var(--dim, #8a8478); }
  #chat .head .tr label { display: inline-flex; gap: 5px; align-items: center; cursor: pointer; }
  #chat select { font: inherit; color: var(--fg, #d8d2c4); background: #0d0d0d; border: 1px solid #3a3528; border-radius: 4px; padding: 2px 4px; }
  #chat select:disabled { opacity: .5; }
  #chat .msg .trnote { display: block; margin-left: 26px; font-size: 11px; color: #5d584e; }
  #chat .msg .trnote[hidden] { display: none; }
  #chat .msg .trnote button { font: inherit; color: var(--dim, #8a8478); background: none; border: 0; padding: 0; cursor: pointer; text-decoration: underline dotted; }
  #chat .msg .crown { margin-left: 3px; font-size: 13px; }
  #chat .namebox .pin { display: inline-flex; gap: 8px; align-items: center; }
  #chat .namebox .pin[hidden] { display: none; }
  #chat .namebox .pin input { width: 7em; }
  #chat .namebox .err { color: #e77; }
  #chat .msg .who { color: var(--accent, #e0a040); font-weight: 600; }
  #chat .msg .t { color: #5d584e; font-size: 11px; margin-left: 6px; }
  #chat .msg .text { color: var(--fg, #d8d2c4); }
  #chat .msg.mine .who { color: #f2d39c; }
  #chat .note { color: var(--dim, #8a8478); font-style: italic; margin: 0 0 6px; }
  #chat .note.bad { color: #e77; }
  #chat form.send { display: flex; gap: 8px; padding: 8px 12px; border-top: 1px solid #2a271f; }
  #chat form.send input { flex: 1; min-width: 0; }
  #chatJump { position: fixed; right: 16px; bottom: 16px; z-index: 4; font: 600 13px system-ui, sans-serif; color: #0d0d0d;
    background: var(--accent, #e0a040); border: 0; border-radius: 18px; padding: 8px 14px; cursor: pointer; box-shadow: 0 2px 10px rgba(0,0,0,.5); }
  #chatJump[hidden] { display: none; }
  #chatJump .n { font-weight: 400; }`;

  const HTML = `
    <div class="head"><h2>Chat</h2><span class="online" id="chatOnline">connecting…</span>
      <span class="tr"><label title="Show other players' messages in your language"><input type="checkbox" id="chatTr"> Translate to</label>
        <select id="chatLang" aria-label="Translate into"></select></span>
      <span class="me">You are <button type="button" id="chatMe" title="Change your name"></button></span>
      <button type="button" id="chatToggle" aria-expanded="true" aria-controls="chatMsgs">Hide chat</button></div>
    <div class="namebox" id="chatNameBox">
      <label for="chatName">Pick a name, or chat as Anonymous:</label>
      <input id="chatName" maxlength="${NAME_MAX}" placeholder="Anonymous" autocomplete="nickname" spellcheck="false">
      <button type="button" id="chatNameSave">Save</button>
      <span class="pin" id="chatPinRow" hidden><label for="chatPin">That name needs a PIN:</label>
        <input id="chatPin" type="password" inputmode="numeric" maxlength="12" autocomplete="off" aria-label="PIN">
        <button type="button" id="chatPinGo">Unlock</button></span>
      <span class="err" id="chatNameErr" role="status"></span>
      <button type="button" class="x" id="chatNameClose" title="Dismiss" aria-label="Dismiss">×</button>
    </div>
    <div class="msgs" id="chatMsgs" role="log" aria-live="polite"></div>
    <form class="send" id="chatForm">
      <input id="chatText" maxlength="${TEXT_MAX}" placeholder="Say something to other players…" autocomplete="off" aria-label="Message" disabled>
      <button type="submit" id="chatSend" disabled>Send</button>
    </form>`;

  function setup() {
    const anchor = document.getElementById('chat');
    if (!anchor) return;
    const DEV = document.documentElement.classList.contains('dev');
    const metaUrl = (document.querySelector('meta[name="fnv-chat"]') || {}).content || '';
    const qUrl = new URLSearchParams(location.search).get('chat');
    const url = (DEV && qUrl) || metaUrl;

    const style = document.createElement('style');
    style.textContent = CSS;
    document.head.appendChild(style);
    anchor.innerHTML = HTML;
    const chatToggle = document.getElementById('chatToggle');
    const setCollapsed = (collapsed) => {
      anchor.classList.toggle('collapsed', collapsed);
      chatToggle.textContent = collapsed ? 'Show chat' : 'Hide chat';
      chatToggle.setAttribute('aria-expanded', String(!collapsed));
      chatToggle.setAttribute('aria-label', collapsed ? 'Show chat' : 'Hide chat');
      try { localStorage.setItem('fnv.chat.collapsed', collapsed ? '1' : '0'); } catch { /* private mode */ }
    };
    let collapsed = false;
    try { collapsed = localStorage.getItem('fnv.chat.collapsed') === '1'; } catch { /* private mode */ }
    setCollapsed(collapsed);
    chatToggle.addEventListener('click', () => setCollapsed(!anchor.classList.contains('collapsed')));
    const $ = (id) => document.getElementById(id);
    const msgs = $('chatMsgs'), online = $('chatOnline'), me = $('chatMe'), nameBox = $('chatNameBox'), nameIn = $('chatName');
    const form = $('chatForm'), textIn = $('chatText'), sendBtn = $('chatSend');

    // the page's keys belong to the chat while typing here, not to the game
    anchor.addEventListener('keydown', (e) => e.stopPropagation());

    // ---- the name -----------------------------------------------------------------------------------------------------
    const store = { get: (k) => { try { return localStorage.getItem(k); } catch { return null; } },
      set: (k, v) => { try { localStorage.setItem(k, v); } catch {} } };
    // the server's test for the reserved name, for asking for the PIN up front (the server decides)
    const FOLD = { 'а': 'a', 'е': 'e', 'ё': 'e', 'о': 'o', 'р': 'p', 'с': 'c', 'х': 'x', 'у': 'y', 'і': 'i', 'ј': 'j', 'ѕ': 's',
      'м': 'm', 'т': 't', 'з': 'z', 'ԁ': 'd', 'ɡ': 'g', 'τ': 't', 'ε': 'e', 'ο': 'o', 'ν': 'v', 'μ': 'm', 'ζ': 'z', 'ʐ': 'z',
      '0': 'o', '1': 'l', '2': 'z', '3': 'e', '5': 's', '7': 't', '$': 's', '@': 'a', '|': 'l' };
    const reserved = (s) => [...String(s).normalize('NFKD').replace(/[\u0300-\u036f]/g, '').toLowerCase()]
      .map((c) => FOLD[c] || c).join('').replace(/rn/g, 'm').replace(/[^a-z]/g, '') === 'metz';
    const pinRow = $('chatPinRow'), pinIn = $('chatPin'), nameErr = $('chatNameErr');
    let owner = false, wanted = '', retext = '';
    let name = (store.get('fnv.chat.name') || '').slice(0, NAME_MAX).trim();
    const showName = () => { me.textContent = name || 'Anonymous'; nameIn.value = name; };
    showName();
    nameBox.hidden = store.get('fnv.chat.nameDismissed') === '1';
    const askPin = (msg) => {
      nameBox.hidden = false; pinRow.hidden = false; nameErr.textContent = msg || '';
      pinIn.value = ''; pinIn.focus();
    };
    const saveName = () => {
      const next = nameIn.value.replace(/\s+/g, ' ').trim().slice(0, NAME_MAX);
      if (reserved(next) && !owner) { wanted = next; askPin(); return; }
      commitName(next);
    };
    const sendAuth = (pin) => { if (ws && ws.readyState === 1) ws.send(JSON.stringify({ type: 'auth', pin })); };
    $('chatPinGo').addEventListener('click', () => {
      if (!ws || ws.readyState !== 1) { nameErr.textContent = 'Not connected to the chat yet.'; return; }
      nameErr.textContent = 'Checking…';
      sendAuth(pinIn.value.trim());
    });
    pinIn.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); $('chatPinGo').click(); } });
    function commitName(next) {
      name = next;
      pinRow.hidden = true; nameErr.textContent = '';
      store.set('fnv.chat.name', name);
      store.set('fnv.chat.nameDismissed', '1');
      showName();
      nameBox.hidden = true;
      if (!textIn.disabled) textIn.focus();
    }
    $('chatNameSave').addEventListener('click', saveName);
    nameIn.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); saveName(); } });
    $('chatNameClose').addEventListener('click', () => { store.set('fnv.chat.nameDismissed', '1'); nameBox.hidden = true; pinRow.hidden = true; nameErr.textContent = ''; });
    me.addEventListener('click', () => { nameBox.hidden = false; nameIn.focus(); nameIn.select(); });

    // ---- translation --------------------------------------------------------------------------------------------------
    // languages offered (both Chrome's on-device translator and the server's model have these)
    const LANGS = ['ar', 'bg', 'bn', 'cs', 'da', 'de', 'el', 'en', 'es', 'fa', 'fi', 'fr', 'he', 'hi', 'hr', 'hu', 'id', 'it', 'ja',
      'ko', 'ms', 'nl', 'no', 'pl', 'pt', 'ro', 'ru', 'sk', 'sr', 'sv', 'sw', 'th', 'tl', 'tr', 'uk', 'ur', 'vi', 'zh'];
    const norm = (l) => { const b = String(l || '').toLowerCase().split(/[-_]/)[0]; return { nb: 'no', nn: 'no', iw: 'he', fil: 'tl', in: 'id' }[b] || b; };
    const browserLang = (navigator.languages || [navigator.language]).map(norm).find((l) => LANGS.includes(l)) || 'en';
    const langNames = (() => { try { return new Intl.DisplayNames([navigator.language || 'en'], { type: 'language' }); } catch { return null; } })();
    const langName = (l) => (langNames && langNames.of(l)) || l;
    const trBox = $('chatTr'), langSel = $('chatLang');
    let trOn = store.get('fnv.chat.translate') !== '0';
    let lang = LANGS.includes(store.get('fnv.chat.lang')) ? store.get('fnv.chat.lang') : browserLang;
    for (const l of [...LANGS].sort((a, b) => langName(a).localeCompare(langName(b)))) {
      const o = document.createElement('option'); o.value = l; o.textContent = langName(l); langSel.appendChild(o);
    }
    trBox.checked = trOn; langSel.value = lang; langSel.disabled = !trOn;
    const recs = new Map();                  // message id -> {m, text, note, orig}
    const tcache = new Map(), tpending = new Set(), trWait = new Map();
    const renderAll = () => { for (const r of recs.values()) render(r); };
    trBox.addEventListener('change', () => { trOn = trBox.checked; langSel.disabled = !trOn; store.set('fnv.chat.translate', trOn ? '1' : '0'); renderAll(); });
    langSel.addEventListener('change', () => { lang = langSel.value; store.set('fnv.chat.lang', lang); renderAll(); });

    // a message is translated unless it is already in the reader's language; a message with no language (older ones)
    // goes to the web translator, which says what it was
    const LATIN = new Set(['cs', 'da', 'de', 'en', 'es', 'fi', 'fr', 'hr', 'hu', 'id', 'it', 'ms', 'nl', 'no', 'pl', 'pt', 'ro', 'sk', 'sv', 'sw', 'tl', 'tr', 'vi']);
    const OTHER_SCRIPT = /[\p{Script=Cyrillic}\p{Script=Greek}\p{Script=Arabic}\p{Script=Hebrew}\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\p{Script=Hangul}\p{Script=Thai}\p{Script=Devanagari}\p{Script=Bengali}]/u;
    const srcOf = (m) => m.lang && !(LATIN.has(m.lang) && OTHER_SCRIPT.test(m.text)) ? m.lang : '';
    let trFailed = 0, trWarned = false;
    function render(r) {
      const m = r.m, key = m.id + '|' + lang;
      const want = trOn && srcOf(m) !== lang;
      let tr = want ? tcache.get(key) : null;
      if (want && tr === undefined && m.tr && typeof m.tr[lang] === 'string') tr = m.tr[lang] ? { text: m.tr[lang], from: m.lang } : null;
      if (want && tr === undefined) translate(r, key);
      const has = !!(want && tr && tr.text && tr.text !== m.text);
      r.text.textContent = ': ' + (has && !r.orig ? tr.text : m.text);
      r.note.hidden = !has;
      if (has) {
        r.note.textContent = (r.orig ? 'Original' : `Translated from ${tr.from ? langName(tr.from) : 'another language'}`) + ' · ';
        const b = document.createElement('button');
        b.type = 'button'; b.textContent = r.orig ? 'show translation' : 'show original';
        b.addEventListener('click', () => { r.orig = !r.orig; render(r); });
        r.note.appendChild(b);
      }
    }
    async function translate(r, key) {
      if (tpending.has(key)) return;
      tpending.add(key);
      const to = lang, src = srcOf(r.m);
      let out = null;
      if (src) { const s = await onDevice(src, to, r.m.text); if (s) out = { text: s, from: src }; }
      if (!out) out = await viaServer(r.m.id, to);
      if (!out) out = await viaWeb(src, to, r.m.text);
      tcache.set(key, out && out.text && out.from !== to ? out : null);
      // nothing could translate it (an extension blocking translate.googleapis.com, say): say so once
      if (!out && ++trFailed >= 3 && !trWarned) { trWarned = true; note('Some messages could not be translated: something in this browser may be blocking translate.googleapis.com.', true); }
      tpending.delete(key);
      if (to === lang) render(r);
    }
    // Chrome's Translator API: no server, nothing leaves the device. A language pair that needs a download is only
    // started from a click (the browser requires it); until then the server translates.
    // the browser's AI APIs can hang (a model not ready): give up after a while and use the server
    const within = (p, ms) => Promise.race([p, new Promise((_, no) => setTimeout(() => no(new Error('timed out')), ms))]);
    const translators = new Map();
    async function onDevice(src, dst, text) {
      if (!('Translator' in self)) return null;
      const k = src + '>' + dst;
      try {
        let t = translators.get(k);
        if (t === undefined) {
          const av = await within(self.Translator.availability({ sourceLanguage: src, targetLanguage: dst }), 2500);
          if (av === 'unavailable') { translators.set(k, null); return null; }
          if (av !== 'available' && !(navigator.userActivation && navigator.userActivation.isActive)) return null;
          t = self.Translator.create({ sourceLanguage: src, targetLanguage: dst });
          translators.set(k, t);
        }
        if (!t) return null;
        return await within((async () => (await t).translate(text))(), 8000);
      } catch { translators.set(k, null); return null; }
    }
    // Google's free web translator, from this browser (it allows any page, and the game page's isolation)
    const GCODE = { zh: 'zh-CN', he: 'iw' };
    async function viaWeb(src, dst, text) {
      const ctl = new AbortController(), timer = setTimeout(() => ctl.abort(), 8000);
      try {
        const q = new URLSearchParams({ client: 'gtx', sl: src ? GCODE[src] || src : 'auto', tl: GCODE[dst] || dst, dt: 't', q: text });
        const res = await fetch('https://translate.googleapis.com/translate_a/single?' + q, { signal: ctl.signal, credentials: 'omit', referrerPolicy: 'no-referrer' });
        if (!res.ok) return null;
        const j = await res.json();
        const out = (j[0] || []).map((s) => s && s[0] || '').join('').trim();
        return out ? { text: out, from: norm(j[2] || src || '') } : null;
      } catch { return null; } finally { clearTimeout(timer); }
    }
    function viaServer(id, to) {
      if (!ws || ws.readyState !== 1) return Promise.resolve('');
      return new Promise((res) => {
        const k = id + '|' + to;
        const done = (v) => { trWait.delete(k); res(v); };
        trWait.set(k, done);
        setTimeout(() => trWait.has(k) && done(''), 12000);
        ws.send(JSON.stringify({ type: 'translate', id, to }));
      });
    }
    // the language of what this player writes
    let detector;
    async function detectLang(text) {
      try {
        if ('LanguageDetector' in self) {
          if (detector === undefined) detector = (await within(self.LanguageDetector.availability(), 1500)) === 'available' ? self.LanguageDetector.create() : null;
          if (detector) {
            const [top] = await within((async () => (await detector).detect(text))(), 1500);
            if (top && top.confidence > 0.6 && top.detectedLanguage !== 'und') return norm(top.detectedLanguage);
          }
        }
      } catch { detector = null; }
      return '';
    }

    // ---- messages -----------------------------------------------------------------------------------------------------
    const flagBase = url ? url.replace(/^ws/, 'http').replace(/\/ws$/, '') + '/flag/' : '';
    const regionNames = (() => { try { return new Intl.DisplayNames([navigator.language || 'en'], { type: 'region' }); } catch { return null; } })();
    const seen = new Set();
    const nearBottom = () => msgs.scrollHeight - msgs.scrollTop - msgs.clientHeight < 40;
    function note(text, bad) {
      const d = document.createElement('p');
      d.className = 'note' + (bad ? ' bad' : '');
      d.textContent = text;
      const stick = nearBottom();
      msgs.appendChild(d);
      if (stick) msgs.scrollTop = msgs.scrollHeight;
    }
    function addMsg(m, scroll = true) {
      if (!m || seen.has(m.id)) return;
      seen.add(m.id);
      const d = document.createElement('div');
      d.className = 'msg' + (m.mine ? ' mine' : '');
      const cc = /^[A-Z]{2}$/.test(m.country || '') ? m.country : '';
      const where = cc ? (regionNames && regionNames.of(cc)) || cc : 'Unknown country';
      if (cc && flagBase) {
        const img = document.createElement('img');
        img.className = 'flag'; img.src = flagBase + cc.toLowerCase() + '.svg'; img.alt = cc; img.title = where;
        img.width = 20; img.height = 15; img.loading = 'lazy'; img.decoding = 'async';
        img.addEventListener('error', () => { const s = document.createElement('span'); s.className = 'noflag'; s.textContent = cc; s.title = where; img.replaceWith(s); });
        d.appendChild(img);
      } else {
        const s = document.createElement('span');
        s.className = 'noflag'; s.textContent = '🌐'; s.title = where;
        d.appendChild(s);
      }
      const who = document.createElement('span');
      who.className = 'who'; who.textContent = m.name || 'Anonymous';
      if (m.owner) { const c = document.createElement('span'); c.className = 'crown'; c.textContent = '👑'; c.title = 'Site owner'; who.append(c); }
      const t = document.createElement('span');
      t.className = 't'; t.textContent = new Date(m.t).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
      const text = document.createElement('span');
      text.className = 'text';
      const trNote = document.createElement('span');
      trNote.className = 'trnote'; trNote.hidden = true;
      d.append(who, text, t, trNote);
      const rec = { m, text, note: trNote, orig: false };
      recs.set(m.id, rec);
      render(rec);
      const stick = nearBottom();
      msgs.appendChild(d);
      if (scroll && stick) msgs.scrollTop = msgs.scrollHeight;
      if (!seenChat) { unread++; jumpCount(); }
    }

    // ---- the connection -----------------------------------------------------------------------------------------------
    let ws = null, started = false, retry = 0, myCountry = '', pendingMine = new Map();
    const setOnline = (n) => { online.textContent = `${n} online`; };
    const enable = (on) => { textIn.disabled = sendBtn.disabled = !on; };
    function connect() {
      enable(false);
      try { ws = new WebSocket(url); } catch (e) { note('Chat is not available: ' + e.message, true); return; }
      ws.onopen = () => { retry = 0; };
      ws.onmessage = (e) => {
        let m;
        try { m = JSON.parse(e.data); } catch { return; }
        if (m.type === 'hello') {
          myCountry = m.you && m.you.country || '';
          owner = false;
          // the owner's name on this browser: unlock it again with the kept PIN
          if (reserved(name) && store.get('fnv.chat.pin')) sendAuth(store.get('fnv.chat.pin'));
          msgs.textContent = ''; seen.clear(); recs.clear();
          for (const h of m.history || []) addMsg(h, false);
          if (!(m.history || []).length) note('No messages yet. Say hello!');
          msgs.scrollTop = msgs.scrollHeight;
          setOnline(m.online);
          enable(true);
        } else if (m.type === 'msg') {
          const key = m.name + '\n' + m.text;
          if (pendingMine.has(key) && m.country === myCountry) { m.mine = true; pendingMine.delete(key); }
          addMsg(m);
        } else if (m.type === 'online') setOnline(m.n);
        else if (m.type === 'auth') {
          if (m.ok) {
            owner = true;
            if (!pinRow.hidden) store.set('fnv.chat.pin', pinIn.value.trim());
            pinIn.value = '';
            if (wanted) { commitName(wanted); wanted = ''; }
            if (retext && !textIn.value) { textIn.value = retext; retext = ''; }
          } else {
            store.set('fnv.chat.pin', '');
            askPin(m.error || 'Wrong PIN.');
            if (!wanted) wanted = name;
          }
        } else if (m.type === 'translation') {
          // {text, from}; text '' with a language: nothing to translate; with none: the server could not
          const f = trWait.get(m.id + '|' + m.to);
          if (f) f(m.text ? { text: m.text, from: m.from || '' } : m.from ? { text: '', from: m.from } : null);
        }
        else if (m.type === 'error' && m.code === 'reserved') { wanted = name; askPin(m.error); }
        else if (m.type === 'error') note(m.error, true);
      };
      ws.onclose = () => {
        enable(false);
        online.textContent = 'reconnecting…';
        const wait = Math.min(30000, 1000 * 2 ** retry++);
        setTimeout(connect, wait);
      };
    }
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const text = textIn.value.replace(/\s+/g, ' ').trim().slice(0, TEXT_MAX);
      if (!text || !ws || ws.readyState !== 1) return;
      textIn.value = '';
      const msgLang = await detectLang(text);
      pendingMine.set((name || 'Anonymous') + '\n' + text, true);
      retext = text;
      ws.send(JSON.stringify({ type: 'msg', name: name || 'Anonymous', text, lang: msgLang }));
    });

    // ---- showing up when scrolled to ----------------------------------------------------------------------------------
    // connect the first time the chat comes into view; while it is out of view a button in the corner counts new
    // messages and scrolls to it
    const jump = document.createElement('button');
    jump.id = 'chatJump'; jump.type = 'button'; jump.hidden = true;
    document.body.appendChild(jump);
    let unread = 0, seenChat = false, inView = false;
    const jumpCount = () => { jump.innerHTML = ''; jump.append('💬 Chat'); if (unread) { const n = document.createElement('span'); n.className = 'n'; n.textContent = ` · ${unread} new`; jump.append(n); } };
    jumpCount();
    jump.addEventListener('click', () => {
      if (anchor.classList.contains('collapsed')) setCollapsed(false);
      anchor.scrollIntoView({ behavior: 'smooth', block: 'end' });
    });
    const start = () => {
      if (started) return;
      started = true;
      if (!url) { online.textContent = 'offline'; note('Chat is not set up on this server.'); return; }
      connect();
    };
    // in view: at least a fifth of the chat (or 120 px of it) on screen
    const check = () => {
      const r = anchor.getBoundingClientRect(), shown = Math.min(r.bottom, innerHeight) - Math.max(r.top, 0);
      inView = shown > Math.min(120, r.height * 0.2);
      if (inView) { start(); seenChat = true; unread = 0; jumpCount(); }
      else seenChat = false;
      // the corner button: once the player has scrolled down the page (not while the game fills the screen at the top)
      jump.hidden = inView || scrollY < 40 || anchor.hidden;
    };
    let queued = false;
    const soon = () => { if (!queued) { queued = true; setTimeout(() => { queued = false; check(); }, 100); } };
    addEventListener('scroll', soon, { passive: true });
    addEventListener('resize', soon);
    if (window.IntersectionObserver) new IntersectionObserver(check, { threshold: [0, 0.2, 0.5] }).observe(anchor);
    soon();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', setup); else setup();
})();
