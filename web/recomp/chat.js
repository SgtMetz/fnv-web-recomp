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
(() => {
  'use strict';
  const NAME_MAX = 24, TEXT_MAX = 300;

  const CSS = `
  #chat { margin: 14px 0 20px; border: 1px solid #3a3528; background: #14130f; border-radius: 6px; display: flex; flex-direction: column; }
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
      <span class="me">You are <button type="button" id="chatMe" title="Change your name"></button></span></div>
    <div class="namebox" id="chatNameBox">
      <label for="chatName">Pick a name, or chat as Anonymous:</label>
      <input id="chatName" maxlength="${NAME_MAX}" placeholder="Anonymous" autocomplete="nickname" spellcheck="false">
      <button type="button" id="chatNameSave">Save</button>
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
    const $ = (id) => document.getElementById(id);
    const msgs = $('chatMsgs'), online = $('chatOnline'), me = $('chatMe'), nameBox = $('chatNameBox'), nameIn = $('chatName');
    const form = $('chatForm'), textIn = $('chatText'), sendBtn = $('chatSend');

    // the page's keys belong to the chat while typing here, not to the game
    anchor.addEventListener('keydown', (e) => e.stopPropagation());

    // ---- the name -----------------------------------------------------------------------------------------------------
    const store = { get: (k) => { try { return localStorage.getItem(k); } catch { return null; } },
      set: (k, v) => { try { localStorage.setItem(k, v); } catch {} } };
    let name = (store.get('fnv.chat.name') || '').slice(0, NAME_MAX).trim();
    const showName = () => { me.textContent = name || 'Anonymous'; nameIn.value = name; };
    showName();
    nameBox.hidden = store.get('fnv.chat.nameDismissed') === '1';
    const saveName = () => {
      name = nameIn.value.replace(/\s+/g, ' ').trim().slice(0, NAME_MAX);
      store.set('fnv.chat.name', name);
      store.set('fnv.chat.nameDismissed', '1');
      showName();
      nameBox.hidden = true;
      if (!textIn.disabled) textIn.focus();
    };
    $('chatNameSave').addEventListener('click', saveName);
    nameIn.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); saveName(); } });
    $('chatNameClose').addEventListener('click', () => { store.set('fnv.chat.nameDismissed', '1'); nameBox.hidden = true; });
    me.addEventListener('click', () => { nameBox.hidden = false; nameIn.focus(); nameIn.select(); });

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
      const t = document.createElement('span');
      t.className = 't'; t.textContent = new Date(m.t).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
      const text = document.createElement('span');
      text.className = 'text'; text.textContent = ': ' + m.text;
      d.append(who, text, t);
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
          msgs.textContent = ''; seen.clear();
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
        else if (m.type === 'error') note(m.error, true);
      };
      ws.onclose = () => {
        enable(false);
        online.textContent = 'reconnecting…';
        const wait = Math.min(30000, 1000 * 2 ** retry++);
        setTimeout(connect, wait);
      };
    }
    form.addEventListener('submit', (e) => {
      e.preventDefault();
      const text = textIn.value.replace(/\s+/g, ' ').trim().slice(0, TEXT_MAX);
      if (!text || !ws || ws.readyState !== 1) return;
      pendingMine.set((name || 'Anonymous') + '\n' + text, true);
      ws.send(JSON.stringify({ type: 'msg', name: name || 'Anonymous', text }));
      textIn.value = '';
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
    jump.addEventListener('click', () => anchor.scrollIntoView({ behavior: 'smooth', block: 'end' }));
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
      jump.hidden = inView || scrollY < 40;
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
