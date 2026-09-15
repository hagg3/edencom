// eden-menu.js — the rebuilt main menu: Main Menu, Load World, New World.
// Requires: window.EdenUI, window.EdenStorage, window.EdenAssets, window.EdenPauseMenu.tick.
// Publishes: window.EdenMenu. See docs/ui.md's dependency graph (audit I2).
//
// These are the three Figma mockup screens (Emod-Menu, Emod-Menu-LoadWorld, Emod-Menu-CreateWorld)
// rebuilt in the DOM on the Eden: Community Edition design system.
//
// HOW IT RELATES TO THE ENGINE'S OWN MENU. Classes/Menu.mm is untouched and still running: it owns
// the world list, the selection, the load state machine, the status bars, the alerts. This is an
// OPAQUE FULL-VIEWPORT OVERLAY on top of it that reads that state and triggers the same
// transitions, through the accessors in web/src/seam/Menu_web.mm — read that file's header for why
// this is an overlay rather than a --wrap (short version: the world-load ladder lives inside
// Menu::render(), so wrapping render away would break loading a world).
//
// Two consequences worth knowing:
//   * Turning this off is a one-line escape hatch — the "Legacy menu" setting just stops the
//     overlay from showing, revealing the original GL menu underneath, fully functional. There is
//     no second implementation to keep working.
//   * The overlay eats all pointer input before it reaches the canvas, so Menu::update's own touch
//     handling never fires while this is up. That is what stops the two menus fighting.
//
// BACKGROUND ART: not shipped with this file. The parallax layers and the wordmark are the
// engine's own textures, read out of the preloaded wasm filesystem as blob URLs by
// eden-assets.js — the same PNGs Menu_background.mm draws, so the rebuilt menu cannot drift from
// the original's art. The three home-tile icons are placeholders taken from the engine's existing
// create/load/share button art, to be replaced with purpose-drawn tiles later.
//
// PLACEHOLDERS: the mockups specify controls the port cannot do yet — per-world Share and Info, and
// the New Dawn 256z height format (blocked by the frozen T_HEIGHT, see ../CLAUDE.md's format
// freeze). Those are rendered, focusable, and honest about doing nothing (see eden-ui.css's
// `.is-placeholder`), rather than omitted — the screen should show what it will be. Get Worlds
// used to be one of these; it now opens a real screen (see renderGetWorlds below) backed by
// eden-worldbrowser.js against the community's static hagg3.github.io/edenarchive catalog, not the
// original edengame.net service (which this fork does not implement a client for).
(function () {
  'use strict';

  // Matches the shared-world upload validator (tools/eden2/UploadMap2.java): only letters, digits,
  // spaces and apostrophes survive a world name round-trip through that service.
  var WORLD_NAME_DISALLOWED = /[^A-Za-z0-9' ]/g;

  var S = {
    open: false,
    screen: 'menu',      // 'menu' | 'load' | 'new' | 'getworlds' | 'autoplay'
    root: null,
    releaseFocus: null,
    loadingEl: null,
    newWorldType: 1,     // 1 = flat, 0 = normal. Index 0 of the generator rail.
    heightFormat: 0,     // 0 = Legacy 64z (default), 1 = New Dawn 256z
    selected: -1,
    // Get Worlds screen state — kept here (not screen-local) so the manifest fetch and any
    // in-flight download survive a re-render, e.g. after a search-box keystroke.
    wbQuery: '',
    wbTag: '',
    wbPage: 0,           // 0-based index into the current filtered result set, PAGE_SIZE per page
    wbList: null,        // cached manifest, once fetched
    wbSelected: null,     // the manifest entry currently shown in the detail pane
    wbDownload: null,     // { entry, busy, progress, done, error }
    // Deep-link ("Play in browser") state — see startAutoPlay below.
    apStatus: null,       // { message, progress } while working, or { error } on failure
  };

  // Deep-link entry point: edenarchive world pages / search results link here as
  // `?playworld=<archive-id>` (the numeric id in a world's `filename`, e.g. "1315348100"), the
  // same query-param scheme eden-st.html already uses for `?build=rel`. Read once at load;
  // `autoplayStarted` makes it fire exactly once even if the player quits back to this menu later
  // in the session (a stale/second boot into the archive world would surprise them).
  var pendingPlayworldId = (function () {
    try {
      var v = new URLSearchParams(location.search).get('playworld');
      return v ? v.trim() : null;
    } catch (e) { return null; }
  })();
  var autoplayStarted = false;

  function M() { return window.Module; }
  function ready() {
    return window.__edenModuleReady && M() && typeof M()._eden_menu_active === 'function';
  }

  function utf8(ptr) {
    var H = M().HEAPU8, end = ptr;
    while (H[end]) end++;
    // The Uint8Array copy is load-bearing in the EDEN_THREADED build (shared memory cannot be
    // handed to TextDecoder) — full reasoning on the canonical copy in eden-settings.js.
    return new TextDecoder().decode(new Uint8Array(H.subarray(ptr, end)));
  }

  // ---------------------------------------------------------------------------------------------
  // Engine accessors (all index-based — see Menu_web.mm's header)
  // ---------------------------------------------------------------------------------------------
  function worldCount() { return M()._eden_menu_world_count(); }
  function worldName(i) { return utf8(M()._eden_menu_world_name(i)); }
  function worldFile(i) { return utf8(M()._eden_menu_world_file(i)); }

  /**
   * The engine's world list, joined with the filesystem metadata the Storage tab already reads.
   *
   * The engine list is authoritative for WHICH worlds exist and their display names; it just has
   * no size or mtime. eden_storage_list_worlds() has both. Joining on the filename avoids a second
   * stat() implementation in Menu_web.mm and guarantees the two lists can never disagree about
   * what exists.
   */
  function worlds() {
    var out = [];
    var meta = {};
    if (window.EdenStorage) {
      window.EdenStorage.listWorlds().forEach(function (w) {
        // Storage rows key on the same on-disk filename Menu's WorldNode carries.
        if (w.file) meta[w.file] = w;
      });
    }
    for (var i = 0; i < worldCount(); i++) {
      var file = worldFile(i);
      out.push({ index: i, name: worldName(i), file: file, meta: meta[file] || null });
    }
    return out;
  }

  function subtitleFor(w) {
    var ES = window.EdenStorage;
    // "06/08/2025 — 18:49   64z" in the mockup. This engine only ever WRITES 64z worlds (the New
    // World height choice is still a placeholder — see this file's header), but as of the 256z
    // read/play/save support landing (docs/eden-file-format.md), an imported or converted world
    // can genuinely be 256z, and w.meta.height (Storage_web.mm) reports which one it really is —
    // hardcoding "64z" here would silently mislabel it.
    if (!w.meta || !ES) return '64z';
    return ES.formatDate(w.meta.mtime) + '   ' + (w.meta.height === 256 ? '256z' : '64z');
  }

  // 256z ("New Dawn") worlds resident-cost ~4x a 64z world (~413 MB of wasm heap vs. ~128 MB,
  // measured pass 67) -- fine on desktop, but the same ceiling that produced audit row A11's
  // iOS Safari OOM, and nothing warns the player before they hit it. Warn, don't refuse: a hard
  // block would also stop a desktop player who's perfectly able to run it. Gated to the touch
  // profile (eden_profile_name(), src/seam/DisplayProfile_web.mm) since desktop is confirmed fine
  // and a warning that fires on every desktop load of a big world would just be noise.
  function warnIfTall(w) {
    if (!w || !w.meta || w.meta.height !== 256) return;
    if (utf8(M()._eden_profile_name()) !== 'touch') return;
    if (typeof showToast === 'function') {
      showToast('This is a tall (256z) world — it needs much more memory and may not load on this device.');
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Screens
  // ---------------------------------------------------------------------------------------------
  function go(screen) {
    S.screen = screen;
    render();
  }

  function playSelected() {
    var idx = M()._eden_menu_selected_index();
    var picked = worlds().filter(function (w) { return w.index === idx; })[0];
    warnIfTall(picked);
    if (M()._eden_menu_play()) render();   // re-render into the loading state
  }

  /** Main Menu — three home tiles over the parallax background, Settings beneath. */
  function renderMainMenu(root) {
    var UI = window.EdenUI;
    var A = window.EdenAssets;

    var logo = A.img(A.NAMES.logo, 'Eden', 'eden-menu__logo');
    root.appendChild(logo);
    root.appendChild(UI.el('div', 'eden-menu__edition', 'Community Edition'));

    var tiles = UI.el('div', 'eden-menu__tiles');
    tiles.appendChild(UI.button({
      size: 'lg', label: 'New World',
      art: A.img(A.NAMES.tileNewWorld, ''),
      onClick: function () { go('new'); },
    }));
    tiles.appendChild(UI.button({
      size: 'lg', label: 'Load World',
      art: A.img(A.NAMES.tileLoadWorld, ''),
      onClick: function () { go('load'); },
    }));
    tiles.appendChild(UI.button({
      size: 'lg', label: 'Get Worlds',
      art: A.img(A.NAMES.tileGetWorlds, ''),
      onClick: function () { go('getworlds'); },
    }));
    root.appendChild(tiles);

    var footer = UI.el('div', 'eden-menu__footer');
    footer.appendChild(UI.button({
      size: 'md', iconImg: A.NAMES.iconSettings, label: 'Settings',
      onClick: function () {
        // Route through the ENGINE's showsettings flag rather than calling EdenSettings.open()
        // directly, so the settings panel closes back through eden_settings_menu_close() and the
        // engine's own state machine stays in step — exactly what the GL Options button did.
        M()._eden_menu_open_settings();
      },
    }));
    root.appendChild(footer);
  }

  /** Load World — the mockup's world list with per-row actions. */
  function renderLoadWorld(root) {
    var UI = window.EdenUI;
    var list = worlds();
    if (S.selected < 0 || S.selected >= list.length) {
      S.selected = M()._eden_menu_selected_index();
      if (S.selected < 0 && list.length) S.selected = 0;
    }

    var win = UI.window({
      title: 'Load World',
      onBack: function () { go('menu'); },
    });

    win.actions.appendChild(importButton());
    win.actions.appendChild(UI.button({
      size: 'sm', label: 'Delete', onClick: function () { confirmDelete(win); },
    }));
    win.actions.appendChild(UI.button({
      size: 'sm', icon: 'download', label: 'Export', onClick: function () { confirmExport(); },
    }));
    win.actions.appendChild(UI.button({
      size: 'sm', label: 'Share', placeholder: true,
      placeholderNote: 'World sharing needs the online service, which is not available in this build',
    }));
    win.actions.appendChild(UI.button({
      size: 'sm', tone: 'positive', icon: 'play', label: 'Play',
      onClick: function () {
        if (S.selected < 0) return;
        M()._eden_menu_select(S.selected);
        playSelected();
      },
    }));

    var pad = UI.el('div', 'eden-content__pad');
    // A single-select list, so it gets listbox semantics rather than being a pile of buttons.
    pad.setAttribute('role', 'listbox');
    pad.setAttribute('aria-label', 'Saved worlds');
    win.content.appendChild(pad);

    if (!list.length) {
      pad.appendChild(UI.section({
        title: 'No worlds yet',
        desc: 'Create one from the New World screen and it will appear here.',
      }));
    }

    list.forEach(function (w) {
      var info = UI.button({
        size: 'square', label: 'i', ariaLabel: 'World info',
        placeholder: true, placeholderNote: 'World details are not available yet',
      });
      var load = UI.button({
        size: 'sm', label: 'Load',
        onClick: function (e) {
          e.stopPropagation();
          M()._eden_menu_select(w.index);
          playSelected();
        },
      });
      var row = UI.listRow({
        title: w.name,
        sub: subtitleFor(w),
        selectable: true,
        selected: w.index === S.selected,
        actions: [info, load],
        onClick: function () {
          S.selected = w.index;
          M()._eden_menu_select(w.index);
          render();
        },
      });
      row.setAttribute('role', 'option');
      pad.appendChild(row);
    });

    root.appendChild(centered(win.root));
  }

  /**
   * Import — same path as the "Import .eden file" row buried in Settings > Storage
   * (eden-settings.js), surfaced here too since Load World is where it's actually useful. Both
   * call the same window.EdenStorage.importFile.
   */
  function importButton() {
    var UI = window.EdenUI;
    var ES = window.EdenStorage;
    var btn = UI.button({ size: 'sm', icon: 'folder-open', label: 'Import' });
    if (!ES) { btn.disabled = true; return btn; }
    // button() appends the label as a bare text node (no wrapper span) — grab it directly so
    // the "Importing…" swap doesn't clobber the icon or the file input appended below.
    var labelNode = Array.prototype.filter.call(btn.childNodes, function (n) {
      return n.nodeType === 3;
    })[0];

    var fileInput = UI.el('input');
    fileInput.type = 'file';
    fileInput.accept = '.eden,.gz';
    fileInput.style.display = 'none';
    btn.appendChild(fileInput);

    function runImport(f) {
      if (!f) return;
      btn.disabled = true;
      if (labelNode) labelNode.textContent = 'Importing…';
      ES.importFile(f, function (ok, err) {
        btn.disabled = false;
        if (labelNode) labelNode.textContent = 'Import';
        if (!ok) { window.alert('Import failed: ' + err); return; }
        S.selected = -1;
        render();
      });
    }

    fileInput.addEventListener('change', function () {
      var f = fileInput.files && fileInput.files[0];
      fileInput.value = '';
      runImport(f);
    });
    btn.addEventListener('click', function () { fileInput.click(); });
    return btn;
  }

  /**
   * Delete confirmation.
   *
   * The engine's own delete goes through showAlertDeleteConfirm(), which the port implements as a
   * deliberate no-op (seam_link_stubs.mm: not confirming a destructive prompt is the safe default
   * when you have no dialog) — so the GL menu's Delete button has never actually deleted anything
   * on web. This asks for confirmation in the DOM and then calls eden_menu_delete_at, making this
   * the port's first working delete.
   */
  function confirmDelete(win) {
    var UI = window.EdenUI;
    if (S.selected < 0) return;
    var name = worldName(S.selected);
    var target = S.selected;

    var scrim = UI.scrim({
      onDismiss: function () { scrim.remove(); if (release) release(); },
    });
    scrim.style.zIndex = 'var(--eden-z-alert)';
    var dlg = UI.window({ title: 'Delete world?', variant: 'dialog', scrollbar: false, role: 'alertdialog' });
    var stack = UI.el('div', 'eden-stack');
    stack.appendChild(UI.el('p', 'eden-stack__text',
      '"' + name + '" will be permanently deleted from this browser. This cannot be undone.'));
    stack.appendChild(UI.button({
      size: 'md', tone: 'danger', icon: 'trash-2', label: 'Delete',
      onClick: function () {
        M()._eden_menu_delete_at(target);
        S.selected = -1;
        scrim.remove();
        if (release) release();
        render();
      },
    }));
    stack.appendChild(UI.button({
      size: 'md', label: 'Cancel',
      onClick: function () { scrim.remove(); if (release) release(); },
    }));
    dlg.content.appendChild(stack);
    scrim.appendChild(dlg.root);
    S.root.appendChild(scrim);
    UI.bindButtonSounds(scrim);
    var release = UI.trapFocus(scrim);
  }

  /**
   * Export — download the selected world's raw .eden file, with a choice between the file exactly
   * as stored and a gzip-compressed copy (see eden-storage.js's exportWorldAt/deflateGzip; there is
   * no reusable encoder for the engine's own RLE variant — docs/eden-file-format.md's "RLE variant"
   * is decode-only and bundled-default-world-only — so gzip is the compressed option here).
   */
  function confirmExport() {
    var UI = window.EdenUI;
    var ES = window.EdenStorage;
    if (S.selected < 0 || !ES) return;
    var name = worldName(S.selected);
    var target = S.selected;

    var scrim = UI.scrim({
      onDismiss: function () { scrim.remove(); if (release) release(); },
    });
    scrim.style.zIndex = 'var(--eden-z-alert)';
    var dlg = UI.window({ title: 'Export world', variant: 'dialog', scrollbar: false, role: 'alertdialog' });
    var stack = UI.el('div', 'eden-stack');
    stack.appendChild(UI.el('p', 'eden-stack__text',
      'Download "' + name + '" as a .eden file you can keep or move to another browser.'));

    var busy = false;
    function doExport(compress) {
      if (busy) return;
      busy = true;
      ES.exportWorldAt(target, compress, function (ok, err) {
        busy = false;
        scrim.remove();
        if (release) release();
        if (!ok) window.alert('Export failed' + (err ? ': ' + err : '.'));
      });
    }

    stack.appendChild(UI.button({
      size: 'md', tone: 'positive', icon: 'download', label: 'Download uncompressed',
      onClick: function () { doExport(false); },
    }));
    stack.appendChild(UI.button({
      size: 'md', icon: 'download', label: 'Download compressed (.gz)',
      disabled: !ES.canCompress(),
      title: ES.canCompress() ? '' : 'Not supported in this browser',
      onClick: function () { doExport(true); },
    }));
    stack.appendChild(UI.button({
      size: 'md', label: 'Cancel',
      onClick: function () { scrim.remove(); if (release) release(); },
    }));
    dlg.content.appendChild(stack);
    scrim.appendChild(dlg.root);
    S.root.appendChild(scrim);
    UI.bindButtonSounds(scrim);
    var release = UI.trapFocus(scrim);
  }

  /**
   * Get Worlds — search/browse the community's static edenarchive catalog and download+import a
   * world. Data comes from eden-worldbrowser.js; this function only owns the DOM.
   *
   * Unlike renderLoadWorld, the search box and tag filter re-render only the results/detail
   * sub-trees (renderResults/renderDetail below) rather than going through the top-level render() —
   * that would rebuild the whole screen (including the search <input>) on every keystroke and lose
   * focus/caret position.
   */
  function renderGetWorlds(root) {
    var UI = window.EdenUI;
    var WB = window.EdenWorldBrowser;

    var win = UI.window({
      title: 'Get Worlds',
      onBack: function () { go('menu'); },
    });

    var pad = UI.el('div', 'eden-content__pad');
    win.content.appendChild(pad);

    var controls = UI.el('div', 'eden-section__body eden-worldbrowser__controls');
    var searchInput = UI.el('input', 'eden-field');
    searchInput.type = 'text';
    searchInput.placeholder = 'Search name, author, tag…';
    searchInput.value = S.wbQuery;
    searchInput.setAttribute('aria-label', 'Search worlds');
    // Audit row 34/C6 (touch text entry): a search query is not a sentence — autocapitalize/
    // autocorrect only fight the player on names/tags that don't match a dictionary word.
    searchInput.autocapitalize = 'off';
    searchInput.autocorrect = 'off';
    searchInput.spellcheck = false;
    searchInput.addEventListener('input', function () {
      S.wbQuery = searchInput.value;
      S.wbPage = 0;
      renderResults();
      win.content.scrollTop = 0;
    });
    var tagSelect = UI.el('select', 'eden-field');
    tagSelect.setAttribute('aria-label', 'Filter by tag');
    tagSelect.addEventListener('change', function () {
      S.wbTag = tagSelect.value;
      S.wbPage = 0;
      renderResults();
      win.content.scrollTop = 0;
    });
    controls.appendChild(searchInput);
    controls.appendChild(tagSelect);
    pad.appendChild(controls);

    var status = UI.el('div', 'eden-section__desc');
    pad.appendChild(status);

    var detailWrap = UI.el('div', 'eden-worldbrowser__detail');
    pad.appendChild(detailWrap);

    var resultsWrap = UI.el('div', 'eden-worldbrowser__results');
    resultsWrap.setAttribute('role', 'listbox');
    resultsWrap.setAttribute('aria-label', 'Downloadable worlds');
    pad.appendChild(resultsWrap);

    // The catalog is a flat ~800-entry list with no server-side paging. The full manifest is
    // fetched up front (EdenWorldBrowser.fetchManifest) and search/tag filtering always runs over
    // all of it — PAGE_SIZE only bounds how many rows we render into the DOM at once, via
    // S.wbPage/prev-next controls below, so a query can still match anywhere in the catalog even
    // though rows are shown a page at a time.
    var PAGE_SIZE = 200;

    function populateTagOptions(list) {
      var current = tagSelect.value || S.wbTag;
      tagSelect.innerHTML = '';
      var allOpt = document.createElement('option');
      allOpt.value = '';
      allOpt.textContent = 'All tags';
      tagSelect.appendChild(allOpt);
      WB.allTags(list).forEach(function (t) {
        var o = document.createElement('option');
        o.value = t;
        o.textContent = t;
        tagSelect.appendChild(o);
      });
      tagSelect.value = current;
    }

    // .eden-content (win.content) is the actual scroll container the results list lives in, not
    // resultsWrap itself — scrolling resultsWrap.scrollTop does nothing.
    function goToPage(n) {
      S.wbPage = n;
      renderResults();
      win.content.scrollTop = 0;
    }

    function makePager(pageCount, filteredLength) {
      var pager = UI.el('div', 'eden-worldbrowser__pager');
      var prevBtn = UI.button({
        size: 'sm', label: 'Previous',
        disabled: S.wbPage <= 0,
        onClick: S.wbPage <= 0 ? null : function () { goToPage(S.wbPage - 1); },
      });
      var label = UI.el('span', 'eden-section__desc',
        'Page ' + (S.wbPage + 1) + ' of ' + pageCount + ' (' + filteredLength + ' matches)');
      var nextBtn = UI.button({
        size: 'sm', label: 'Next',
        disabled: S.wbPage >= pageCount - 1,
        onClick: S.wbPage >= pageCount - 1 ? null : function () { goToPage(S.wbPage + 1); },
      });
      pager.appendChild(prevBtn);
      pager.appendChild(label);
      pager.appendChild(nextBtn);
      return pager;
    }

    function renderResults() {
      resultsWrap.innerHTML = '';
      if (!S.wbList) return;
      var filtered = WB.search(S.wbList, S.wbQuery, S.wbTag);
      if (!filtered.length) {
        resultsWrap.appendChild(UI.section({
          title: 'No worlds found', desc: 'Try a different search or tag.',
        }));
        return;
      }
      var pageCount = Math.max(1, Math.ceil(filtered.length / PAGE_SIZE));
      if (S.wbPage >= pageCount) S.wbPage = pageCount - 1;
      if (S.wbPage < 0) S.wbPage = 0;
      var start = S.wbPage * PAGE_SIZE;
      if (pageCount > 1) resultsWrap.appendChild(makePager(pageCount, filtered.length));
      filtered.slice(start, start + PAGE_SIZE).forEach(function (entry) {
        var row = UI.listRow({
          title: entry.worldname || entry.filename,
          sub: [entry.author, entry.publishdate, entry.filesize].filter(Boolean).join('   '),
          desc: (entry.tags || []).join(', '),
          selectable: true,
          selected: S.wbSelected === entry,
          onClick: function () {
            S.wbSelected = entry;
            renderResults();
            renderDetail();
          },
        });
        row.setAttribute('role', 'option');
        resultsWrap.appendChild(row);
      });
      if (pageCount > 1) resultsWrap.appendChild(makePager(pageCount, filtered.length));
    }

    function startDownload(entry) {
      S.wbDownload = { entry: entry, busy: true, progress: 0 };
      renderDetail();
      WB.downloadAndImport(entry, {
        onProgress: function (pct) {
          if (S.wbDownload && S.wbDownload.entry === entry) {
            S.wbDownload.progress = pct;
            renderDetail();
          }
        },
      }, function (ok, err) {
        S.wbDownload = { entry: entry, busy: false, done: ok, error: ok ? null : err };
        if (ok) { go('load'); return; }   // matches drag-and-drop import's own landing spot
        renderDetail();
      });
    }

    // The download button lives in the titlebar (win.actions) rather than inline in detailWrap —
    // with up to 200 rows per page, an inline button can be a page-plus of scrolling away from the
    // row the user just clicked. The titlebar stays visible next to Back/the title regardless of
    // scroll position.
    function renderHeaderAction() {
      win.actions.innerHTML = '';
      var entry = S.wbSelected;
      if (!entry) return;
      var dl = S.wbDownload && S.wbDownload.entry === entry ? S.wbDownload : null;
      win.actions.appendChild(UI.button({
        size: 'sm', tone: 'positive', label: dl && dl.busy ? 'Downloading…' : 'Download',
        disabled: !!(dl && dl.busy),
        onClick: dl && dl.busy ? null : function () { startDownload(entry); },
      }));
    }

    function renderDetail() {
      detailWrap.innerHTML = '';
      renderHeaderAction();
      var entry = S.wbSelected;
      if (!entry) return;

      detailWrap.appendChild(UI.section({ title: entry.worldname || entry.filename }));

      var img = document.createElement('img');
      img.className = 'eden-worldbrowser__preview';
      img.alt = '';
      img.src = WB.previewUrl(entry);
      // Most worlds published in the last 1-2 years have no preview (a known bug on the archive
      // site itself) — hide the broken-image icon rather than showing it.
      img.addEventListener('error', function () { img.remove(); });
      detailWrap.appendChild(img);

      var dl = S.wbDownload && S.wbDownload.entry === entry ? S.wbDownload : null;

      if (dl && dl.busy) {
        var bar = UI.el('div', 'eden-progress');
        var fill = UI.el('div', 'eden-progress__fill');
        fill.style.width = (dl.progress || 0) + '%';
        bar.appendChild(fill);
        bar.setAttribute('role', 'progressbar');
        bar.setAttribute('aria-valuemin', '0');
        bar.setAttribute('aria-valuemax', '100');
        bar.setAttribute('aria-valuenow', String(dl.progress || 0));
        detailWrap.appendChild(bar);
      } else if (dl && dl.error) {
        detailWrap.appendChild(UI.el('p', 'eden-stack__text', 'Download failed: ' + dl.error));
      }
    }

    if (S.wbList) {
      populateTagOptions(S.wbList);
      renderResults();
    } else {
      status.textContent = 'Loading world list…';
      WB.fetchManifest(function (err, list) {
        // The player may have backed out (or the manifest may resolve after a second, cached call
        // from a re-visit) before this fires — only touch the DOM if this screen is still current.
        if (S.screen !== 'getworlds' || !detailWrap.isConnected) return;
        if (err) {
          status.textContent = 'Could not load the world archive (' + (err.message || err) + ').';
          return;
        }
        S.wbList = list;
        status.textContent = '';
        populateTagOptions(list);
        renderResults();
      });
    }
    renderDetail();

    root.appendChild(centered(win.root));
  }

  /** New World — name field, generator-type rail, height format. */
  function renderNewWorld(root) {
    var UI = window.EdenUI;
    var TYPES = [
      { icon: 'square', label: 'Flat', flat: 1, title: 'New Flat World' },
      { icon: 'mountain', label: 'Normal', flat: 0, title: 'New Normal World' },
      // The engine has exactly two generators (FileManager::genflat). Biome is in the mockup as a
      // future third; it is shown so the screen matches the design, but as a genuinely disabled
      // control (greyed, no hover, no press, out of the tab order) rather than a `placeholder`,
      // because there is nothing here to explain yet — it is simply switched off.
      { icon: 'trees', label: 'Biome', disabled: true },
    ];
    var active = TYPES.filter(function (t) { return t.flat === S.newWorldType; })[0] || TYPES[0];

    var win = UI.window({
      title: active.title,
      rail: true,
      railLabel: 'World type',
      onBack: function () { go('menu'); },
    });

    var nameInput = UI.el('input', 'eden-field');
    nameInput.type = 'text';
    nameInput.placeholder = 'Enter text...';
    nameInput.maxLength = 60;      // the C-side name buffer is 128 bytes; 60 chars is safe in UTF-8
    nameInput.id = 'eden-new-world-name';
    // Audit row 34/C6 (touch text entry): a world name is often not a real word (or is several
    // short ones) — autocapitalize/autocorrect/spellcheck would fight the player and, on some
    // mobile keyboards, autocorrect can silently substitute a different word entirely.
    nameInput.autocapitalize = 'off';
    nameInput.autocorrect = 'off';
    nameInput.spellcheck = false;
    // The shared-world service only accepts A-Z/0-9/space/' (tools/eden2/UploadMap2.java strips
    // anything else on upload) — filter live so a name never silently gets mangled later.
    nameInput.addEventListener('input', function () {
      var filtered = nameInput.value.replace(WORLD_NAME_DISALLOWED, '');
      if (filtered !== nameInput.value) {
        var pos = nameInput.selectionStart - (nameInput.value.length - filtered.length);
        nameInput.value = filtered;
        nameInput.setSelectionRange(pos, pos);
      }
    });

    function create() {
      // Park the generator choice so showAlertWorldType() consumes it instead of raising a modal
      // the player has already answered on this screen (see Menu_web.mm's world-type section).
      M()._eden_menu_set_pending_world_type(S.newWorldType);
      // Same one-shot parking for height: consumed by FileManager::probeWorldHeight() the moment
      // this world is first loaded (Play, right after create). 64z stays the default -- this only
      // ever requests 256 when the player picked "New Dawn 256z" above.
      M()._eden_menu_set_pending_world_height(S.heightFormat === 1 ? 256 : 64);
      writeNameBuffer(nameInput.value.trim());
      var idx = M()._eden_menu_create_world();
      if (idx < 0) return;
      S.selected = idx;
      playSelected();
    }

    win.actions.appendChild(UI.button({
      size: 'sm', tone: 'positive', icon: 'play', label: 'Play', onClick: create,
    }));

    TYPES.forEach(function (t) {
      var tab = UI.button({
        size: 'tab', icon: t.icon, ariaLabel: t.label, title: t.label,
        disabled: t.disabled,
        onClick: t.disabled ? null : function () { S.newWorldType = t.flat; render(); },
      });
      tab.setAttribute('role', 'tab');
      tab.setAttribute('aria-selected', (!t.disabled && t.flat === S.newWorldType) ? 'true' : 'false');
      win.rail.appendChild(tab);
    });
    UI.syncRailTabIndex(win.rail);
    UI.railKeyNav(win.rail, function (i) {
      if (TYPES[i].disabled) return;
      S.newWorldType = TYPES[i].flat;
      render();
    });

    var pad = UI.el('div', 'eden-content__pad');
    win.content.appendChild(pad);

    var nameSection = UI.section({ title: 'World name' });
    // The visible pixel-font heading IS the field's label; wiring them together means a screen
    // reader announces "World name, edit text" instead of an unlabelled box.
    nameSection.querySelector('.eden-section__title').id = 'eden-new-world-name-label';
    nameInput.setAttribute('aria-labelledby', 'eden-new-world-name-label');
    pad.appendChild(nameSection);
    var fieldWrap = UI.el('div', 'eden-section__body');
    fieldWrap.appendChild(nameInput);
    pad.appendChild(fieldWrap);
    // Enter in the name field is "create", which is what anyone who just typed a name will press.
    nameInput.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') { e.preventDefault(); create(); }
    });

    pad.appendChild(UI.section({
      title: 'Height format',
      desc: 'New Dawn worlds have a much greater height limit but create a larger file size ' +
        'and cannot be opened by builds that only understand the legacy 64-block format.',
    }));
    var seg = UI.el('div', 'eden-seg eden-section__body');
    seg.setAttribute('role', 'radiogroup');
    seg.setAttribute('aria-label', 'Height format');
    var legacy = UI.button({
      size: 'md', label: 'Legacy 64z',
      onClick: function () { S.heightFormat = 0; render(); },
    });
    legacy.setAttribute('role', 'radio');
    legacy.setAttribute('aria-checked', S.heightFormat === 0 ? 'true' : 'false');
    if (S.heightFormat === 0) legacy.classList.add('is-active');
    var newDawn = UI.button({
      size: 'md', label: 'New Dawn 256z',
      onClick: function () { S.heightFormat = 1; render(); },
    });
    newDawn.setAttribute('role', 'radio');
    newDawn.setAttribute('aria-checked', S.heightFormat === 1 ? 'true' : 'false');
    if (S.heightFormat === 1) newDawn.classList.add('is-active');
    seg.appendChild(legacy);
    seg.appendChild(newDawn);
    pad.appendChild(seg);

    root.appendChild(centered(win.root));
  }

  /**
   * Copy a JS string into the C-owned name buffer.
   *
   * This is the port's only string-INTO-wasm path. It writes UTF-8 bytes straight into HEAPU8 at a
   * pointer C owns, rather than adding _malloc/_free to the export list for one field — see
   * web/docs/ui.md "Passing data across the JS/wasm boundary".
   */
  function writeNameBuffer(text) {
    var ptr = M()._eden_menu_name_buffer();
    var cap = M()._eden_menu_name_buffer_size();
    // Belt-and-suspenders: the live input filter should already have stripped these, but never let
    // an unfiltered name (e.g. a future caller) reach the save file only to get mangled on upload.
    var bytes = new TextEncoder().encode((text || '').replace(WORLD_NAME_DISALLOWED, ''));
    var n = Math.min(bytes.length, cap - 1);
    // Never split a multi-byte sequence: back off to the last byte that starts a code point.
    while (n > 0 && (bytes[n] & 0xC0) === 0x80) n--;
    M().HEAPU8.set(bytes.subarray(0, n), ptr);
    M().HEAPU8[ptr + n] = 0;
  }

  /** Loading state. The engine is mid-load; nothing here is interactive. */
  function renderLoading(root) {
    var UI = window.EdenUI;
    var win = UI.window({ title: 'Loading world', variant: 'dialog', scrollbar: false });
    var stack = UI.el('div', 'eden-stack');
    var pct = UI.el('p', 'eden-stack__text', 'Generating terrain…');
    // The percentage updates every frame from tick(); announce it politely rather than not at all,
    // but don't spam — `aria-live="polite"` on a value that changes this fast is announced by
    // screen readers at their own pace, not per update.
    pct.setAttribute('role', 'status');
    pct.setAttribute('aria-live', 'polite');
    stack.appendChild(pct);
    var bar = UI.el('div', 'eden-progress');
    var fill = UI.el('div', 'eden-progress__fill');
    bar.appendChild(fill);
    bar.setAttribute('role', 'progressbar');
    bar.setAttribute('aria-valuemin', '0');
    bar.setAttribute('aria-valuemax', '100');
    stack.appendChild(bar);
    win.content.appendChild(stack);
    root.appendChild(centered(win.root));
    S.loadingEl = { pct: pct, fill: fill, bar: bar };
  }

  /**
   * Deep-link ("Play in browser") flow, driven by `pendingPlayworldId`. Reuses a locally-saved
   * copy if one already exists (matches renderLoadWorld's own filename convention, `<id>.eden`),
   * otherwise pulls the entry from the same archive manifest Get Worlds uses and downloads it —
   * same eden-worldbrowser.js path startDownload() takes, just without the browse/search UI in
   * front of it. Ends by selecting the world and calling playSelected(), same as clicking Play.
   */
  function startAutoPlay(id) {
    var file = id + '.eden';
    var existing = worlds().filter(function (w) { return w.file === file; })[0];
    if (existing) {
      M()._eden_menu_select(existing.index);
      playSelected();
      return;
    }

    S.apStatus = { message: 'Loading the archive catalog…' };
    render();
    var WB = window.EdenWorldBrowser;
    WB.fetchManifest(function (err, list) {
      if (S.screen !== 'autoplay') return;   // player backed out (Go to menu) before this resolved
      if (err) {
        failAutoPlay('Could not reach the world archive (' + ((err && err.message) || err) + ').');
        return;
      }
      var entry = list.filter(function (e) { return WB.idFor(e) === id; })[0];
      if (!entry) {
        failAutoPlay('World "' + id + '" was not found in the archive.');
        return;
      }
      S.apStatus = { message: 'Downloading "' + (entry.worldname || id) + '"…', progress: 0 };
      render();
      WB.downloadAndImport(entry, {
        onProgress: function (pct) {
          if (S.screen !== 'autoplay' || !S.apStatus) return;
          S.apStatus.progress = pct;
          render();
        },
      }, function (ok, downloadErr) {
        if (S.screen !== 'autoplay') return;
        if (!ok) { failAutoPlay('Download failed: ' + downloadErr); return; }
        // eden-worldbrowser.js's importFile already triggered eden_storage_reload_worlds(), so the
        // engine's own list (worlds(), backed by Menu_web.mm) already includes the new file here.
        var imported = worlds().filter(function (w) { return w.file === file; })[0];
        if (!imported) {
          failAutoPlay('The world downloaded but could not be found to play.');
          return;
        }
        M()._eden_menu_select(imported.index);
        playSelected();
      });
    });
  }

  function failAutoPlay(message) {
    S.apStatus = { error: message };
    render();
  }

  /** Deep-link progress/error screen — see startAutoPlay. Not interactive except on failure. */
  function renderAutoPlay(root) {
    var UI = window.EdenUI;
    var st = S.apStatus || {};
    var win = UI.window({
      title: st.error ? 'Could not open this world' : 'Opening world from the archive',
      variant: 'dialog', scrollbar: false,
    });
    var stack = UI.el('div', 'eden-stack');

    if (st.error) {
      stack.appendChild(UI.el('p', 'eden-stack__text', st.error));
      stack.appendChild(UI.button({
        size: 'md', label: 'Go to menu',
        onClick: function () { S.apStatus = null; go('menu'); },
      }));
    } else {
      var msg = UI.el('p', 'eden-stack__text', st.message || 'Working…');
      msg.setAttribute('role', 'status');
      msg.setAttribute('aria-live', 'polite');
      stack.appendChild(msg);
      if (typeof st.progress === 'number') {
        var bar = UI.el('div', 'eden-progress');
        var fill = UI.el('div', 'eden-progress__fill');
        fill.style.width = st.progress + '%';
        bar.appendChild(fill);
        bar.setAttribute('role', 'progressbar');
        bar.setAttribute('aria-valuemin', '0');
        bar.setAttribute('aria-valuemax', '100');
        bar.setAttribute('aria-valuenow', String(st.progress));
        stack.appendChild(bar);
      }
    }

    win.content.appendChild(stack);
    root.appendChild(centered(win.root));
  }

  function centered(node) {
    var wrap = window.EdenUI.el('div', 'eden-menu__center');
    wrap.appendChild(node);
    return wrap;
  }

  // ---------------------------------------------------------------------------------------------
  // Shell
  // ---------------------------------------------------------------------------------------------
  function buildBackground(root) {
    var UI = window.EdenUI;
    var A = window.EdenAssets;
    // Back to front, mirroring Menu_background::render's own layer order.
    var sky = UI.el('div', 'eden-menu__layer eden-menu__layer--sky');
    A.applyBackground(sky, A.NAMES.skyMagenta);
    var pin = UI.el('div', 'eden-menu__layer eden-menu__layer--pinwheel');
    A.applyBackground(pin, A.NAMES.pinwheel);
    pin.style.backgroundSize = '100% 100%';
    var mountains = UI.el('div', 'eden-menu__layer eden-menu__layer--strip eden-menu__layer--mountains');
    A.applyBackground(mountains, A.NAMES.mountains);
    var trees = UI.el('div', 'eden-menu__layer eden-menu__layer--strip eden-menu__layer--trees');
    A.applyBackground(trees, A.NAMES.treesLeft);
    var ground = UI.el('div', 'eden-menu__layer eden-menu__layer--strip eden-menu__layer--ground');
    A.applyBackground(ground, A.NAMES.ground);
    root.appendChild(sky);
    root.appendChild(pin);
    root.appendChild(mountains);
    root.appendChild(trees);
    root.appendChild(ground);
  }

  function render() {
    if (!S.root) return;
    if (S.releaseFocus) { S.releaseFocus(); S.releaseFocus = null; }
    S.loadingEl = null;

    // Keep the background layers (they animate; rebuilding them would restart every animation and
    // re-decode the art) and replace only the foreground.
    var fg = S.root.querySelector('.eden-menu__fg');
    if (fg) fg.remove();
    fg = window.EdenUI.el('div', 'eden-menu__fg');
    S.root.appendChild(fg);

    if (M()._eden_menu_loading()) {
      renderLoading(fg);
      return;   // no focus trap: nothing in the loading state is interactive
    }
    if (S.screen === 'load') renderLoadWorld(fg);
    else if (S.screen === 'new') renderNewWorld(fg);
    else if (S.screen === 'getworlds') renderGetWorlds(fg);
    else if (S.screen === 'autoplay') renderAutoPlay(fg);
    else renderMainMenu(fg);

    S.releaseFocus = window.EdenUI.trapFocus(fg);
  }

  function show() {
    if (S.root) return;
    window.EdenUI.ensureCSS();
    var root = window.EdenUI.el('div', 'eden-menu');
    root.setAttribute('role', 'dialog');
    root.setAttribute('aria-modal', 'true');
    root.setAttribute('aria-label', 'Eden main menu');
    buildBackground(root);
    document.body.appendChild(root);
    S.root = root;
    window.EdenUI.bindButtonSounds(root);
    // Escape backs out one screen, which is what the Back button does — a menu that traps you on a
    // sub-screen unless you find the right button is a bad menu.
    root.addEventListener('keydown', function (e) {
      if (e.key !== 'Escape') return;
      if (S.screen !== 'menu') { e.stopPropagation(); go('menu'); }
    });
    render();
  }

  function hide() {
    if (!S.root) return;
    if (S.releaseFocus) { S.releaseFocus(); S.releaseFocus = null; }
    S.root.remove();
    S.root = null;
    S.loadingEl = null;
  }

  // ---------------------------------------------------------------------------------------------
  // Engine polling — same pattern as EdenPauseMenu.tick(), driven from eden-st.html's rAF loop.
  // ---------------------------------------------------------------------------------------------
  var wasLoading = false;

  function tick() {
    if (!ready()) return;
    var active = M()._eden_menu_active() !== 0;
    if (active && !S.open) {
      S.open = true;
      if (pendingPlayworldId && !autoplayStarted) {
        autoplayStarted = true;
        S.screen = 'autoplay';
        show();
        startAutoPlay(pendingPlayworldId);
      } else {
        S.screen = 'menu';
        show();
      }
    }
    if (!active && S.open) { S.open = false; hide(); }
    if (!S.open) return;

    // A load starting or finishing changes which screen is showing; everything else is
    // user-driven, so this is the only thing the poll has to watch.
    var loading = M()._eden_menu_loading() !== 0;
    if (loading !== wasLoading) { wasLoading = loading; render(); }

    if (loading && S.loadingEl) {
      var pct = M()._eden_menu_load_percent();
      S.loadingEl.fill.style.width = pct + '%';
      S.loadingEl.bar.setAttribute('aria-valuenow', String(pct));
      S.loadingEl.pct.textContent = 'Generating terrain… ' + pct + '%';
    }
  }

  window.EdenMenu = {
    tick: tick,
    isOpen: function () { return S.open; },
  };
})();
