// eden-ui.js — the runtime half of the Eden design system (public/eden-ui.css is the other half).
// Requires: window.EdenAssets, window.EdenIcons (loaded before this in eden-st.html). Publishes:
// window.EdenUI. See docs/ui.md's dependency graph (audit I2).
//
// Every DOM surface in the port builds itself from the factories here rather than hand-assembling
// elements and class strings, so a change to what a "button" or a "window" is happens in exactly
// one place. Nothing in this file knows anything about the engine: it takes options and returns
// detached DOM. Wiring to wasm lives in the screen files (eden-settings.js, eden-menu.js, ...).
//
// THREE THINGS THIS FILE OWNS THAT ARE EASY TO MISS:
//
//   1. `--u`, the scale unit. eden-ui.css authors every dimension as `calc(N * var(--u))` against
//      the 783x587 mockup frame. This file recomputes `--u` from the viewport so the UI stays
//      proportionally chunky from a phone in landscape up to a 4K monitor — a fixed-pixel UI
//      authored at iPad size would be a postage stamp on a desktop.
//
//   2. The scrollbar is a REAL control. The mockups make the chunky scrollbar the primary scroll
//      affordance for the content box, so `bindScrollbar` mirrors the content's scroll metrics
//      into the thumb and implements drag/track-click. The native scrollbar is hidden in CSS;
//      wheel, touch and keyboard scrolling are untouched.
//
//   3. Focus management for modal surfaces (`trapFocus`). The panels this replaces were modal in
//      appearance only — Tab walked straight out of them into the page behind. Anything that
//      calls itself `aria-modal` needs to actually contain focus.
(function () {
  'use strict';

  // The mockup frame every dimension in eden-ui.css is authored against.
  var BASE_W = 783, BASE_H = 587;
  // Floor of 1 = never smaller than the mockup's own pixels (below that the pixel font stops
  // being legible and the 2px keylines fall apart). Ceiling keeps a 4K monitor from turning the
  // menu into billboard art.
  var U_MIN = 1, U_MAX = 2.4;

  function updateScale() {
    var vw = window.innerWidth || BASE_W;
    var vh = window.innerHeight || BASE_H;
    var u = Math.min(vw / BASE_W, vh / BASE_H);
    u = Math.max(U_MIN, Math.min(U_MAX, u));
    // Rounded to 1/20 so a drag-resize doesn't rewrite every computed length on every frame.
    u = Math.round(u * 20) / 20;
    document.documentElement.style.setProperty('--u', u + 'px');
    return u;
  }

  // The stylesheet is a real file rather than an injected string (the old settings panel injected
  // ~200 lines of CSS from JS). Screens still call ensureCSS() so load order never matters and a
  // page that forgets the <link> still works.
  function ensureCSS() {
    if (document.getElementById('eden-ui-css')) return;
    var link = document.createElement('link');
    link.id = 'eden-ui-css';
    link.rel = 'stylesheet';
    // Resolved against this script's own URL, not the page's, so the port keeps working when
    // served under a path prefix (e.g. a GitHub Pages project site).
    link.href = new URL('eden-ui.css', document.currentScript ? document.currentScript.src : location.href).href;
    document.head.appendChild(link);
  }

  // --- tiny DOM helper ------------------------------------------------------------------------
  function el(tag, className, text) {
    var n = document.createElement(tag);
    if (className) n.className = className;
    if (text != null) n.textContent = text;
    return n;
  }

  function icon(name, opts) {
    return window.EdenIcons ? window.EdenIcons.svg(name, opts) : el('span');
  }

  // --- button ---------------------------------------------------------------------------------
  /**
   * opts:
   *   label       text (omitted for icon-only buttons)
   *   size        'sm' | 'square' | 'tab' | 'md' | 'lg'   (default 'md')
   *   tone        'positive' | 'danger'
   *   icon        vector icon name (eden-icons.js), rendered before the label
   *   iconImg     bundle-asset name (eden-assets.js NAMES) for the buttons that carry the game's
   *               OWN raster icon art instead of a vector glyph. Takes precedence over `icon`.
   *   art         an <img> (or any node) for the 'lg' home tiles — see eden-assets.js
   *   onClick     click handler
   *   ariaLabel   required when there is no visible label
   *   placeholder true = a control the mockups specify but the port can't do yet. Stays focusable
   *               and explains itself instead of being an inert dead button (see eden-ui.css).
   *   disabled    true = a genuinely inert control: no hover, no press, no keyboard reach. Unlike
   *               `placeholder` (which stays focusable so it can explain itself), this is for
   *               controls that should read as switched off — e.g. the third New World generator
   *               tab. Both greys out; only this one leaves the tab order.
   *   title       tooltip
   */
  function button(opts) {
    opts = opts || {};
    var b = el('button', 'eden-btn' + (opts.size ? ' eden-btn--' + opts.size : ' eden-btn--md') +
      (opts.tone ? ' eden-btn--' + opts.tone : '') + (opts.className ? ' ' + opts.className : ''));
    b.type = 'button';
    if (opts.art) {
      var art = el('span', 'eden-btn__art');
      art.appendChild(opts.art);
      b.appendChild(art);
    }
    if (opts.iconImg && window.EdenAssets) {
      b.appendChild(window.EdenAssets.img(opts.iconImg, '', 'eden-icon eden-icon--art'));
    } else if (opts.icon) {
      b.appendChild(icon(opts.icon));
    }
    if (opts.label != null) b.appendChild(document.createTextNode(opts.label));
    if (opts.ariaLabel) b.setAttribute('aria-label', opts.ariaLabel);
    if (opts.title) b.title = opts.title;

    if (opts.disabled) {
      // A real `disabled`, which is what actually kills hover, :active, click and Tab reach — the
      // CSS greying alone would still leave a button that lights up under the cursor.
      b.disabled = true;
      return b;
    }
    if (opts.placeholder) {
      b.classList.add('is-placeholder');
      // Screen-reader users get the same information sighted users get from the greyed styling:
      // the control exists, it is deliberate, and it does nothing yet.
      b.setAttribute('aria-disabled', 'true');
      var why = opts.placeholderNote || 'Not implemented yet';
      b.title = (opts.title ? opts.title + ' — ' : '') + why;
      var sr = el('span', 'eden-sr-only', ' (' + why + ')');
      b.appendChild(sr);
      // aria-disabled alone doesn't stop activation, so swallow it here. Keeping the element
      // focusable (no `disabled`) is the entire point — a `disabled` button is skipped by Tab and
      // announces nothing.
      b.addEventListener('click', function (e) { e.preventDefault(); e.stopPropagation(); });
      return b;
    }
    if (opts.onClick) b.addEventListener('click', opts.onClick);
    return b;
  }

  // --- toggle ---------------------------------------------------------------------------------
  // A real <input type="checkbox"> at full size and zero opacity over two painted halves. That is
  // what keeps Space/Enter, :checked, :disabled, focus and screen-reader semantics working with no
  // JS at all — the two <span>s are pure decoration.
  function toggle(opts) {
    opts = opts || {};
    var wrap = el('label', 'eden-toggle');
    var input = el('input');
    input.type = 'checkbox';
    input.checked = !!opts.checked;
    if (opts.disabled) input.disabled = true;
    if (opts.ariaLabel) input.setAttribute('aria-label', opts.ariaLabel);
    if (opts.onChange) input.addEventListener('change', function () { opts.onChange(input.checked, input); });
    wrap.appendChild(input);
    wrap.appendChild(el('span', 'eden-toggle__half eden-toggle__half--on'));
    wrap.appendChild(el('span', 'eden-toggle__half eden-toggle__half--off'));
    wrap.input = input;
    return wrap;
  }

  // --- list row -------------------------------------------------------------------------------
  /** opts: title, desc (prose, sans face), sub (metadata), actions (node|array), muted, selectable */
  function listRow(opts) {
    opts = opts || {};
    var row = el(opts.selectable ? 'button' : 'div',
      'eden-listrow' + (opts.selectable ? ' eden-listrow--selectable' : '') + (opts.muted ? ' is-muted' : ''));
    if (opts.selectable) {
      row.type = 'button';
      if (opts.selected) row.setAttribute('aria-selected', 'true');
      if (opts.onClick) row.addEventListener('click', opts.onClick);
    }
    var meta = el('div', 'eden-listrow__meta');
    if (opts.title != null) meta.appendChild(el('div', 'eden-listrow__title', opts.title));
    if (opts.desc) meta.appendChild(el('div', 'eden-listrow__desc', opts.desc));
    if (opts.sub) meta.appendChild(el('div', 'eden-listrow__sub', opts.sub));
    row.appendChild(meta);
    if (opts.actions) {
      var acts = el('div', 'eden-listrow__actions');
      (Array.isArray(opts.actions) ? opts.actions : [opts.actions]).forEach(function (a) {
        if (a) acts.appendChild(a);
      });
      row.appendChild(acts);
    }
    row.metaEl = meta;
    return row;
  }

  function section(opts) {
    opts = opts || {};
    var s = el('div', 'eden-section');
    if (opts.title) s.appendChild(el('div', 'eden-section__title', opts.title));
    if (opts.desc) s.appendChild(el('div', 'eden-section__desc', opts.desc));
    return s;
  }

  // --- window ---------------------------------------------------------------------------------
  /**
   * Builds the floating panel every screen except the main menu is made of:
   *
   *   +--------------------------------------------------+
   *   | [Back]           Title            [actions...]   |   .eden-titlebar
   *   +------+-------------------------------+-----------+
   *   | rail |          content              | scrollbar |   .eden-window__body
   *   +------+-------------------------------+-----------+
   *
   * Returns { root, titlebar, title, lead, actions, body, rail, content, scrollbar }.
   * `rail` and `scrollbar` are only created when asked for — a dialog needs neither.
   */
  function windowPanel(opts) {
    opts = opts || {};
    var root = el('div', 'eden-window' + (opts.variant ? ' eden-window--' + opts.variant : '') +
      (opts.className ? ' ' + opts.className : ''));
    root.setAttribute('role', opts.role || 'dialog');
    root.setAttribute('aria-modal', 'true');
    if (opts.title) root.setAttribute('aria-label', opts.title);

    var bar = el('div', 'eden-titlebar');
    var lead = el('div', 'eden-titlebar__lead');
    var titleEl = el('h2', 'eden-titlebar__title', opts.title || '');
    var actions = el('div', 'eden-titlebar__actions');
    if (opts.onBack) {
      lead.appendChild(button({
        size: 'sm', label: opts.backLabel || 'Back', onClick: opts.onBack,
      }));
    }
    bar.appendChild(lead);
    bar.appendChild(titleEl);
    bar.appendChild(actions);
    root.appendChild(bar);

    var body = el('div', 'eden-window__body');
    var rail = null, scrollbar = null;
    if (opts.rail) {
      rail = el('div', 'eden-tabrail');
      rail.setAttribute('role', 'tablist');
      rail.setAttribute('aria-orientation', 'vertical');
      if (opts.railLabel) rail.setAttribute('aria-label', opts.railLabel);
      body.appendChild(rail);
    }
    var content = el('div', 'eden-content');
    body.appendChild(content);
    if (opts.scrollbar !== false) {
      scrollbar = el('div', 'eden-scrollbar');
      var thumb = el('div', 'eden-scrollbar__thumb');
      for (var i = 0; i < 3; i++) thumb.appendChild(el('div', 'eden-scrollbar__grip'));
      scrollbar.appendChild(thumb);
      body.appendChild(scrollbar);
      bindScrollbar(content, scrollbar);
    }
    root.appendChild(body);

    return {
      root: root, titlebar: bar, title: titleEl, lead: lead, actions: actions,
      body: body, rail: rail, content: content, scrollbar: scrollbar,
    };
  }

  // --- scrollbar ------------------------------------------------------------------------------
  /**
   * Makes `bar` reflect and drive `content`'s scroll position.
   *
   * The native scrollbar is hidden (eden-ui.css) because the mockups draw their own; every native
   * scroll INPUT still works — wheel, touch drag, PageUp/Down, arrows, find-in-page — because
   * `content` remains a genuinely scrollable element. This only adds a visual and a drag handle.
   */
  function bindScrollbar(content, bar) {
    var thumb = bar.querySelector('.eden-scrollbar__thumb');
    if (!thumb) return;

    function sync() {
      var view = content.clientHeight;
      var total = content.scrollHeight;
      if (total <= view + 1) {
        // Nothing to scroll. The track stays (the mockups always show it) but the thumb fills it
        // and stops advertising a drag.
        bar.classList.add('is-inert');
        thumb.style.height = '100%';
        thumb.style.top = '0px';
        return;
      }
      bar.classList.remove('is-inert');
      var trackH = bar.clientHeight;
      var h = Math.max(24, Math.round(trackH * (view / total)));
      var maxTop = trackH - h;
      var frac = content.scrollTop / (total - view);
      thumb.style.height = h + 'px';
      thumb.style.top = Math.round(maxTop * Math.min(1, Math.max(0, frac))) + 'px';
    }

    content.addEventListener('scroll', sync, { passive: true });
    // Content can change height without scrolling (a tab switch, a row appearing) — watch for it
    // rather than re-syncing on a timer.
    if (window.ResizeObserver) {
      var ro = new ResizeObserver(sync);
      ro.observe(content);
      // The first child wrapper is what actually grows; observing only the (fixed-height) content
      // box would miss content changes entirely.
      if (content.firstElementChild) ro.observe(content.firstElementChild);
      bar._edenObserver = ro;
    }

    var dragging = false, startY = 0, startScroll = 0;
    thumb.addEventListener('pointerdown', function (e) {
      dragging = true;
      startY = e.clientY;
      startScroll = content.scrollTop;
      thumb.setPointerCapture(e.pointerId);
      e.preventDefault();
      e.stopPropagation();
    });
    thumb.addEventListener('pointermove', function (e) {
      if (!dragging) return;
      var trackH = bar.clientHeight;
      var h = thumb.offsetHeight;
      var maxTop = trackH - h;
      if (maxTop <= 0) return;
      var range = content.scrollHeight - content.clientHeight;
      content.scrollTop = startScroll + ((e.clientY - startY) / maxTop) * range;
    });
    function endDrag(e) {
      if (!dragging) return;
      dragging = false;
      try { thumb.releasePointerCapture(e.pointerId); } catch (err) {}
    }
    thumb.addEventListener('pointerup', endDrag);
    thumb.addEventListener('pointercancel', endDrag);

    // Click above/below the thumb pages, the way a real scrollbar does.
    bar.addEventListener('pointerdown', function (e) {
      if (e.target === thumb || thumb.contains(e.target)) return;
      var rect = bar.getBoundingClientRect();
      var above = (e.clientY - rect.top) < thumb.offsetTop;
      content.scrollTop += (above ? -1 : 1) * content.clientHeight * 0.9;
    });

    // Content is usually empty at bind time; sync once it has been filled.
    requestAnimationFrame(sync);
    bar.sync = sync;
    return sync;
  }

  // --- tab rail keyboard navigation -----------------------------------------------------------
  /**
   * Applies the ARIA tabs pattern to a vertical rail: exactly one tab is in the tab order (roving
   * tabindex), Up/Down move between them, Home/End jump to the ends. Without this a 7-tab rail
   * costs 7 Tab presses to walk past, and arrow keys — which is what a screen-reader user will
   * actually try on a tablist — do nothing.
   *
   * `onSelect(index)` is called when the selection changes. The index is into ALL role="tab"
   * elements including disabled ones, so it always lines up with the caller's own tab array;
   * arrow/Home/End navigation steps OVER disabled tabs rather than parking focus on one.
   */
  function railKeyNav(rail, onSelect) {
    function tabs() { return Array.prototype.slice.call(rail.querySelectorAll('[role="tab"]')); }
    rail.addEventListener('keydown', function (e) {
      var items = tabs();
      var i = items.indexOf(document.activeElement);
      if (i < 0) return;
      var n = items.length;
      // Walk in `step` from `from` until a tab that can actually take focus; give up after a full
      // lap so a rail of entirely-disabled tabs can't spin here.
      function seek(from, step) {
        for (var k = 0; k < n; k++) {
          var j = ((from + k * step) % n + n) % n;
          if (!items[j].disabled) return j;
        }
        return -1;
      }
      var next = -1;
      if (e.key === 'ArrowDown' || e.key === 'ArrowRight') next = seek(i + 1, 1);
      else if (e.key === 'ArrowUp' || e.key === 'ArrowLeft') next = seek(i - 1, -1);
      else if (e.key === 'Home') next = seek(0, 1);
      else if (e.key === 'End') next = seek(n - 1, -1);
      else return;
      e.preventDefault();
      if (next < 0) return;
      items[next].focus();
      // Follow-focus selection, which is the expected behaviour for a tablist whose panels are
      // cheap to render (all of ours are).
      if (onSelect) onSelect(next);
    });
  }

  /** Keeps roving tabindex honest after a re-render. */
  function syncRailTabIndex(rail) {
    Array.prototype.slice.call(rail.querySelectorAll('[role="tab"]')).forEach(function (t) {
      t.tabIndex = t.getAttribute('aria-selected') === 'true' ? 0 : -1;
    });
  }

  // --- modal focus management -------------------------------------------------------------------
  var FOCUSABLE = 'button:not([disabled]), [href], input:not([disabled]), select, textarea, ' +
    '[tabindex]:not([tabindex="-1"])';

  /**
   * Contains Tab inside `root` and restores focus to wherever it was when released.
   *
   * Returns a release function. Call it in the screen's close path — leaking a trap leaves the
   * page's Tab key permanently captured by a detached node.
   */
  function trapFocus(root, initial) {
    var previouslyFocused = document.activeElement;
    function onKey(e) {
      if (e.key !== 'Tab') return;
      var items = Array.prototype.slice.call(root.querySelectorAll(FOCUSABLE))
        .filter(function (n) { return n.offsetParent !== null || n === document.activeElement; });
      if (!items.length) return;
      var first = items[0], last = items[items.length - 1];
      if (e.shiftKey && document.activeElement === first) {
        e.preventDefault(); last.focus();
      } else if (!e.shiftKey && document.activeElement === last) {
        e.preventDefault(); first.focus();
      }
    }
    root.addEventListener('keydown', onKey);
    // Deferred: the caller usually appends `root` in the same tick, and focusing a node that is
    // not yet in the document is a no-op.
    requestAnimationFrame(function () {
      var target = initial || root.querySelector(FOCUSABLE);
      if (target && document.contains(target)) target.focus();
    });
    return function release() {
      root.removeEventListener('keydown', onKey);
      if (previouslyFocused && document.contains(previouslyFocused)) {
        try { previouslyFocused.focus(); } catch (e) {}
      }
    };
  }

  function scrim(opts) {
    opts = opts || {};
    var s = el('div', 'eden-scrim' + (opts.className ? ' ' + opts.className : ''));
    if (opts.id) s.id = opts.id;
    if (opts.onDismiss) {
      // Only a press that both starts AND ends on the scrim itself dismisses — otherwise a drag
      // that begins on a slider inside the panel and ends outside it reads as "close".
      var downOnScrim = false;
      s.addEventListener('pointerdown', function (e) { downOnScrim = (e.target === s); });
      s.addEventListener('pointerup', function (e) {
        if (downOnScrim && e.target === s) opts.onDismiss();
        downOnScrim = false;
      });
    }
    return s;
  }

  /**
   * Plays the engine's own menu-button press/release samples for DOM controls.
   *
   * Delegated on a container rather than bound per button, so buttons added later get it for
   * free. Pass `skip` for controls that own a different sound (the settings toggles use
   * eden_play_switch_toggle_sound instead — see Settings_web.mm for why that one is reserved).
   */
  function bindButtonSounds(rootEl, skipSelector) {
    function play(down, e) {
      var b = e.target.closest && e.target.closest('button, .eden-toggle');
      if (!b || b.disabled) return;
      if (skipSelector && b.closest(skipSelector)) return;
      var M = window.Module;
      if (window.__edenModuleReady && M && M._eden_play_menu_button_sound) {
        M._eden_play_menu_button_sound(down);
      }
    }
    rootEl.addEventListener('pointerdown', function (e) { play(1, e); });
    rootEl.addEventListener('pointerup', function (e) { play(0, e); });
  }

  updateScale();
  window.addEventListener('resize', updateScale);
  window.addEventListener('orientationchange', updateScale);
  ensureCSS();

  window.EdenUI = {
    ensureCSS: ensureCSS,
    updateScale: updateScale,
    el: el,
    icon: icon,
    button: button,
    toggle: toggle,
    listRow: listRow,
    section: section,
    window: windowPanel,
    scrim: scrim,
    bindScrollbar: bindScrollbar,
    railKeyNav: railKeyNav,
    syncRailTabIndex: syncRailTabIndex,
    trapFocus: trapFocus,
    bindButtonSounds: bindButtonSounds,
  };
})();
