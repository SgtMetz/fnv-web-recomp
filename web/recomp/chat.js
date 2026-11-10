// Chat (web/recomp/index.html): a room where players talk to each other, under the game. A classic script; it
// connects only once the chat has been scrolled into view, and keeps no connection open for a player who never looks.
//
// The server is a WebSocket at the URL in <meta name="fnv-chat"> (empty: no chat, the panel says so). A deployment
// points it at its own room; the development page also takes ?chat=ws://... . Protocol, from the server:
// {type: 'hello', history, online, you: {country}}, {type: 'you', name, owner}, {type: 'msg', id, name, country, text,
// t}, {type: 'dm', id, name, country, to, text, t}, {type: 'online', n, names}, {type: 'error', error}; to it:
// {type: 'join', token}, {type: 'msg', text}, {type: 'dm', to, text}. The country (an ISO code, or '') is the
// server's, from where the connection comes from; flags come from <server>/flag/<cc>.svg.
//
// Names come from the server, by country: "Russian #3" is the third player from Russia. A secret token kept on this
// browser (fnv.chat.token) gets the same name back next time. Clicking a name @mentions it; the ✉ after it (or a
// name in the "n online" list) starts a private message, which only that player sees and the server does not keep.
// Messages that @mention this player are highlighted, and the corner button counts them.
//
// The site owner's name (Metz) needs a PIN, entered in the box under "You are ..."; the server checks it
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
  const TEXT_MAX = 300;
  const WELCOME_BOT = 'Lichtsteiner #1';
  const WELCOME_LINK = 'https://example.com/';

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
  #chat .msg .who { color: var(--accent, #e0a040); font-weight: 600; background: none; border: 0; padding: 0; font: inherit; font-weight: 600; cursor: pointer; }
  #chat .msg .who:hover { text-decoration: underline; }
  #chat .msg .dmb, #chat .roster .dmb { background: none; border: 0; padding: 0 3px; margin-left: 2px; cursor: pointer; opacity: .55; font-size: 12px; }
  #chat .msg .dmb:hover, #chat .roster .dmb:hover { opacity: 1; }
  #chat .msg .at { color: #9cc7f2; }
  #chat .msg .at.me { color: #0d0d0d; background: var(--accent, #e0a040); border-radius: 3px; padding: 0 2px; }
  #chat .msg.mention { background: rgba(224,160,64,.10); box-shadow: inset 3px 0 0 var(--accent, #e0a040); padding-left: 4px; }
  #chat .msg.dm { background: rgba(120,90,200,.13); box-shadow: inset 3px 0 0 #9a7be0; padding-left: 4px; }
  #chat .msg .dmlabel { color: #b9a2f0; font-size: 12px; margin-right: 4px; }
  #chat .head .online { font: inherit; color: var(--dim, #8a8478); background: none; border: 0; padding: 0; cursor: pointer; text-decoration: underline dotted; }
  #chat .roster { display: flex; flex-wrap: wrap; gap: 6px 14px; padding: 8px 12px; background: #1b1913; border-bottom: 1px solid #2a271f; }
  #chat .roster[hidden] { display: none; }
  #chat .roster .who { font: inherit; color: var(--accent, #e0a040); background: none; border: 0; padding: 0; cursor: pointer; }
  #chat .namebox .why { color: var(--dim, #8a8478); flex-basis: 100%; }
  #chat form.send .dmto { display: inline-flex; align-items: center; gap: 4px; color: #b9a2f0; white-space: nowrap; }
  #chat form.send .dmto[hidden] { display: none; }
  #chat form.send .dmto button { padding: 0 6px; }
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
    <div class="head"><h2>Chat</h2><button type="button" class="online" id="chatOnline" title="Who is online" aria-expanded="false" aria-controls="chatRoster">connecting…</button>
      <span class="tr"><label title="Show other players' messages in your language"><input type="checkbox" id="chatTr"> Translate to</label>
        <select id="chatLang" aria-label="Translate into"></select></span>
      <span class="me">You are <button type="button" id="chatMe" title="Where your name comes from"></button></span>
      <button type="button" id="chatToggle" aria-expanded="true" aria-controls="chatMsgs">Hide chat</button></div>
    <div class="roster" id="chatRoster" hidden></div>
    <div class="namebox" id="chatNameBox" hidden>
      <span class="why">Names come from your country: the third player from Russia is Russian #3. Click a name to @mention
        them, or ✉ to send them a private message.</span>
      <span class="pin" id="chatPinRow"><label for="chatPin">Site owner PIN:</label>
        <input id="chatPin" type="password" inputmode="numeric" maxlength="12" autocomplete="off" aria-label="PIN">
        <button type="button" id="chatPinGo">Unlock</button></span>
      <span class="err" id="chatNameErr" role="status"></span>
      <button type="button" class="x" id="chatNameClose" title="Dismiss" aria-label="Dismiss">×</button>
    </div>
    <div class="msgs" id="chatMsgs" role="log" aria-live="polite"></div>
    <form class="send" id="chatForm">
      <span class="dmto" id="chatDmTo" hidden>✉ <b id="chatDmName"></b>
        <button type="button" id="chatDmX" title="Back to everyone" aria-label="Stop private message">×</button></span>
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
    const msgs = $('chatMsgs'), online = $('chatOnline'), me = $('chatMe'), nameBox = $('chatNameBox');
    const form = $('chatForm'), textIn = $('chatText'), sendBtn = $('chatSend');

    // the page's keys belong to the chat while typing here, not to the game
    anchor.addEventListener('keydown', (e) => e.stopPropagation());

    // ---- the name -----------------------------------------------------------------------------------------------------
    const store = { get: (k) => { try { return localStorage.getItem(k); } catch { return null; } },
      set: (k, v) => { try { localStorage.setItem(k, v); } catch {} } };
    // the server hands out the name ({type: 'you'}); this browser's secret token keeps it the same next time
    let token = store.get('fnv.chat.token') || '';
    if (!/^[A-Za-z0-9_-]{16,64}$/.test(token)) {
      const b = crypto.getRandomValues(new Uint8Array(24));
      token = btoa(String.fromCharCode(...b)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
      store.set('fnv.chat.token', token);
    }
    const pinIn = $('chatPin'), nameErr = $('chatNameErr');
    let owner = false, name = '';
    const showName = () => { me.textContent = name || '…'; };
    showName();
    const sendAuth = (pin) => { if (ws && ws.readyState === 1) ws.send(JSON.stringify({ type: 'auth', pin })); };
    $('chatPinGo').addEventListener('click', () => {
      if (!ws || ws.readyState !== 1) { nameErr.textContent = 'Not connected to the chat yet.'; return; }
      nameErr.textContent = 'Checking…';
      sendAuth(pinIn.value.trim());
    });
    pinIn.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); $('chatPinGo').click(); } });
    $('chatNameClose').addEventListener('click', () => { nameBox.hidden = true; nameErr.textContent = ''; });
    me.addEventListener('click', () => { nameBox.hidden = !nameBox.hidden; });

    // ---- @mentions and private messages -------------------------------------------------------------------------------
    // names look like "Russian #3", "South African #12", "Wanderer #1", or the owner's Metz
    const MENTION = /@(\p{L}[\p{L}' -]{0,30}? #\d+|Metz\b)/gu;
    const isMe = (n) => !!name && String(n).toLowerCase() === name.toLowerCase();
    const mentionsMe = (text) => [...String(text).matchAll(MENTION)].some((x) => isMe(x[1]));
    let dmTo = '';
    const dmBox = $('chatDmTo'), dmName = $('chatDmName');
    const setDm = (to) => {
      dmTo = to && !isMe(to) ? to : '';
      dmBox.hidden = !dmTo; dmName.textContent = dmTo;
      textIn.placeholder = dmTo ? `Private message to ${dmTo}…` : 'Say something to other players…';
      if (!textIn.disabled) textIn.focus();
    };
    $('chatDmX').addEventListener('click', () => setDm(''));
    const mention = (n) => {
      const v = textIn.value, add = '@' + n + ' ';
      textIn.value = (v && !/\s$/.test(v) ? v + ' ' : v) + add;
      if (!textIn.disabled) textIn.focus();
    };
    // a name you can click (@mention) with a ✉ (private message) after it
    const nameButtons = (n, ownerMark) => {
      const who = document.createElement('button');
      who.type = 'button'; who.className = 'who'; who.textContent = n; who.title = `Mention ${n}`;
      who.addEventListener('click', () => mention(n));
      if (ownerMark) { const c = document.createElement('span'); c.className = 'crown'; c.textContent = '👑'; c.title = 'Site owner'; who.append(c); }
      const out = [who];
      if (!isMe(n)) {
        const dm = document.createElement('button');
        dm.type = 'button'; dm.className = 'dmb'; dm.textContent = '✉'; dm.title = `Private message to ${n}`;
        dm.setAttribute('aria-label', `Private message to ${n}`);
        dm.addEventListener('click', () => setDm(n));
        out.push(dm);
      }
      return out;
    };
    // the message text with @names coloured (yours highlighted)
    const fillText = (el, s) => {
      el.textContent = ': ';
      let at = 0;
      for (const x of String(s).matchAll(MENTION)) {
        el.append(s.slice(at, x.index));
        const sp = document.createElement('span');
        sp.className = 'at' + (isMe(x[1]) ? ' me' : ''); sp.textContent = x[0];
        el.append(sp);
        at = x.index + x[0].length;
      }
      el.append(s.slice(at));
    };
    const roster = $('chatRoster');
    let rosterNames = [];
    const showRoster = () => {
      roster.textContent = '';
      for (const n of rosterNames) { const s = document.createElement('span'); s.append(...nameButtons(n, n === 'Metz')); roster.append(s); }
      if (!rosterNames.length) roster.textContent = 'Nobody else here yet.';
    };
    online.addEventListener('click', () => { roster.hidden = !roster.hidden; online.setAttribute('aria-expanded', String(!roster.hidden)); showRoster(); });

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
      if (m.type === 'dm' && m.name === WELCOME_BOT && m.text === WELCOME_LINK) {
        const link = document.createElement('a');
        link.href = WELCOME_LINK;
        link.textContent = WELCOME_LINK;
        link.target = '_blank';
        link.rel = 'noopener noreferrer';
        r.text.replaceChildren(': ', link);
        r.note.hidden = true;
        return;
      }
      const want = trOn && srcOf(m) !== lang;
      let tr = want ? tcache.get(key) : null;
      if (want && tr === undefined && m.tr && typeof m.tr[lang] === 'string') tr = m.tr[lang] ? { text: m.tr[lang], from: m.lang } : null;
      if (want && tr === undefined) translate(r, key);
      const has = !!(want && tr && tr.text && tr.text !== m.text);
      fillText(r.text, has && !r.orig ? tr.text : m.text);
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
    // everything shown since the last hello, to draw again once the server says who this player is
    let shown = [];
    function redraw() {
      const all = shown; shown = [];
      msgs.textContent = ''; seen.clear(); recs.clear();
      for (const m of all) addMsg(m, false);
      if (!all.length) note('No messages yet. Say hello!');
      msgs.scrollTop = msgs.scrollHeight;
    }
    function addMsg(m, scroll = true) {
      if (!m || seen.has(m.id)) return;
      seen.add(m.id);
      shown.push(m);
      if (shown.length > 400) shown.splice(0, shown.length - 400);
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
      const mine = isMe(m.name);
      if (mine) d.classList.add('mine');
      // a private message: "✉ to Russian #3" (yours) or "✉ private" (to you)
      if (m.type === 'dm') {
        d.classList.add('dm');
        const l = document.createElement('span');
        l.className = 'dmlabel'; l.textContent = mine ? `✉ to ${m.to}` : '✉ private';
        if (mine) { l.style.cursor = 'pointer'; l.title = `Private message to ${m.to}`; l.addEventListener('click', () => setDm(m.to)); }
        d.appendChild(l);
      } else if (!mine && mentionsMe(m.text)) d.classList.add('mention');
      const nb = nameButtons(m.name || 'Anonymous', m.owner);
      const t = document.createElement('span');
      t.className = 't'; t.textContent = new Date(m.t).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
      const text = document.createElement('span');
      text.className = 'text';
      const trNote = document.createElement('span');
      trNote.className = 'trnote'; trNote.hidden = true;
      d.append(...nb, text, t, trNote);
      const rec = { m, text, note: trNote, orig: false };
      recs.set(m.id, rec);
      render(rec);
      const stick = nearBottom();
      msgs.appendChild(d);
      if (scroll && stick) msgs.scrollTop = msgs.scrollHeight;
      if (!seenChat && scroll) { unread++; if (!mine && (m.type === 'dm' || mentionsMe(m.text))) forYou++; jumpCount(); }
    }

    // ---- the connection -----------------------------------------------------------------------------------------------
    let ws = null, started = false, retry = 0;
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
          owner = false;
          ws.send(JSON.stringify({ type: 'join', token }));
          // the owner on this browser: unlock the owner's name again with the kept PIN
          if (store.get('fnv.chat.pin')) sendAuth(store.get('fnv.chat.pin'));
          // keep this tab's private messages across a reconnect (the server does not store them)
          const dms = shown.filter((x) => x.type === 'dm');
          shown = [...(m.history || []), ...dms].sort((a, b) => a.t - b.t);
          redraw();
          setOnline(m.online);
        } else if (m.type === 'you') {
          // the name the server gave this browser (or the owner's, after the PIN)
          const changed = m.name !== name;
          name = m.name || ''; owner = !!m.owner;
          showName();
          if (changed) redraw();
          enable(true);
        } else if (m.type === 'msg' || m.type === 'dm') {
          addMsg(m);
        } else if (m.type === 'online') {
          setOnline(m.n);
          rosterNames = (m.names || []).filter((n) => !isMe(n));
          if (!roster.hidden) showRoster();
        } else if (m.type === 'auth') {
          if (m.ok) {
            if (pinIn.value.trim()) store.set('fnv.chat.pin', pinIn.value.trim());
            pinIn.value = ''; nameErr.textContent = ''; nameBox.hidden = true;
          } else {
            store.set('fnv.chat.pin', '');
            // a stale kept PIN fails quietly; a typed one says why
            if (pinIn.value) nameErr.textContent = m.error || 'Wrong PIN.';
          }
        } else if (m.type === 'translation') {
          // {text, from}; text '' with a language: nothing to translate; with none: the server could not
          const f = trWait.get(m.id + '|' + m.to);
          if (f) f(m.text ? { text: m.text, from: m.from || '' } : m.from ? { text: '', from: m.from } : null);
        }
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
      ws.send(JSON.stringify(dmTo ? { type: 'dm', to: dmTo, text, lang: msgLang } : { type: 'msg', text, lang: msgLang }));
    });

    // ---- showing up when scrolled to ----------------------------------------------------------------------------------
    // connect the first time the chat comes into view; while it is out of view a button in the corner counts new
    // messages and scrolls to it
    const jump = document.createElement('button');
    jump.id = 'chatJump'; jump.type = 'button'; jump.hidden = true;
    document.body.appendChild(jump);
    let unread = 0, forYou = 0, seenChat = false, inView = false;
    const jumpCount = () => { jump.innerHTML = ''; jump.append('💬 Chat'); if (unread) { const n = document.createElement('span'); n.className = 'n'; n.textContent = ` · ${unread} new` + (forYou ? ` · ${forYou} for you` : ''); jump.append(n); } };
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
      if (inView) { start(); seenChat = true; unread = 0; forYou = 0; jumpCount(); }
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
