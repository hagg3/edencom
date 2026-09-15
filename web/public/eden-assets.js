// eden-assets.js — hands the DOM UI the engine's own art, with zero extra bundle weight.
// Requires: Module.FS (the wasm module must already be booted/preloaded). Publishes:
// window.EdenAssets. See docs/ui.md's dependency graph (audit I2).
//
// THE PROBLEM: the main-menu mockup's background is the painted voxel landscape, the sunburst and
// the "EDEN" wordmark. Those already ship — CMakeLists.txt --preload-file's media/ipad_menu into
// the wasm virtual filesystem at /bundle/media/ipad_menu, which is where Menu_background.mm draws
// them from. But a DOM <img>/background-image can't read a path inside the Emscripten FS; it needs
// a URL. Copying the PNGs into public/assets/ would work and is what a normal web app would do,
// but it would ship ~2 MB of art TWICE — once in eden.data, once as loose files.
//
// THE FIX: read the bytes back out of the FS and wrap them in a blob: URL, cached by path. The
// browser then treats them like any other image. One decode, no duplication, and the DOM menu is
// guaranteed to be showing exactly the same art as the GL menu it replaces.
//
// TIMING: this only works after the module's preloaded package has mounted, which is well before
// the menu is reachable (the menu is the first screen the player sees, but it renders after
// main()). `ready()` is the gate; every screen already polls from eden-st.html's rAF loop, so
// there is nothing to await.
(function () {
  'use strict';

  var cache = Object.create(null);   // fs path -> blob: URL
  var missing = Object.create(null); // fs path -> true, so a miss is logged once, not every frame

  function fs() {
    return window.FS || (window.Module && window.Module.FS) || null;
  }

  function ready() {
    var F = fs();
    return !!(window.__edenModuleReady && F && typeof F.readFile === 'function');
  }

  // The engine is configured as a retina/"IS_IPAD" profile (see src/seam/EAGLView_web.mm), so the
  // ipad~ variants are the ones it actually draws — and for several of these (sky, ground,
  // mountains, treelayers) they are the ONLY variant that exists at all. Probe the ipad~ path
  // first and fall back to the plain one, mirroring the engine's own resource lookup.
  var ROOTS = ['/bundle/media/ipad_menu/ipad~', '/bundle/media/menu/', '/bundle/media/ui/'];

  /**
   * Resolve a bundle asset to a blob: URL, or null if it isn't there (yet).
   *
   * `name` is a bare filename as it appears in media/, e.g. "sky.png" — the ipad~ prefix and the
   * containing directory are this function's business, not the caller's.
   */
  function url(name) {
    if (cache[name]) return cache[name];
    if (missing[name]) return null;
    var F = fs();
    if (!F) return null;
    for (var i = 0; i < ROOTS.length; i++) {
      var path = ROOTS[i] + name;
      try {
        var data = F.readFile(path);           // Uint8Array
        var blob = new Blob([data], { type: mimeFor(name) });
        cache[name] = URL.createObjectURL(blob);
        return cache[name];
      } catch (e) {
        /* not at this root — try the next */
      }
    }
    // Cache the miss: without this, a menu that asks for a missing layer every frame would retry
    // three failing FS reads per frame forever.
    missing[name] = true;
    console.warn('[eden-assets] not found in bundle: ' + name);
    return null;
  }

  function mimeFor(name) {
    if (/\.jpe?g$/i.test(name)) return 'image/jpeg';
    if (/\.gif$/i.test(name)) return 'image/gif';
    return 'image/png';
  }

  /**
   * An <img> for a bundle asset. Returns the element immediately and fills in `src` when the FS
   * is ready, so callers can build their DOM in one pass without awaiting anything.
   */
  function img(name, alt, className) {
    var el = document.createElement('img');
    el.alt = alt || '';
    if (className) el.className = className;
    el.decoding = 'async';
    var src = url(name);
    if (src) {
      el.src = src;
    } else {
      // Retry on the next frame until the module is up. Bounded so a genuinely missing asset
      // doesn't spin a rAF loop for the life of the page.
      var tries = 0;
      (function retry() {
        if (el.src) return;
        var s = url(name);
        if (s) { el.src = s; return; }
        if (++tries > 240) return;   // ~4s at 60fps, then give up quietly
        requestAnimationFrame(retry);
      })();
    }
    return el;
  }

  /** `background-image` value for a bundle asset, or '' if unavailable. */
  function cssUrl(name) {
    var u = url(name);
    return u ? 'url("' + u + '")' : '';
  }

  /**
   * Apply bundle art to an element's background-image, retrying until the FS is up.
   * Used for the menu's tiled parallax strips, which are backgrounds rather than <img>s because
   * they need background-repeat/background-position-x animation.
   */
  function applyBackground(el, name) {
    var tries = 0;
    (function attempt() {
      var v = cssUrl(name);
      if (v) { el.style.backgroundImage = v; return; }
      if (++tries > 240) return;
      requestAnimationFrame(attempt);
    })();
  }

  window.EdenAssets = {
    ready: ready,
    url: url,
    img: img,
    cssUrl: cssUrl,
    applyBackground: applyBackground,
    // The names the menu uses, in one place so a rename in media/ has a single fix site.
    NAMES: {
      sky: 'sky.png',
      skyMagenta: 'sky_magenta.png',
      pinwheel: 'pinwheel.png',
      mountains: 'mountains.png',
      treesLeft: 'treelayerleft.png',
      treesRight: 'treelayerright.png',
      ground: 'ground.png',
      logo: 'eden_menu_header.png',
      // Home-tile art. These are the engine's pre-rendered 3D block icons — the one place this
      // design system keeps raster (see eden-icons.js's header). Placeholders for now: the user
      // is supplying purpose-drawn art for these three tiles later, and only these paths change.
      tileNewWorld: 'create_world.png',
      tileLoadWorld: 'load_world.png',
      tileGetWorlds: 'share_world.png',
      // Action icons. These are the engine's own HUD/menu button art (media/ui), used where the
      // player asked for the original game's iconography instead of the Lucide vector set — the
      // pause menu's actions and the Settings button on both menus. They resolve through the
      // '/bundle/media/ui/' root, so the ipad~ ones must carry that prefix in the name.
      iconSave: 'ipad~save.png',
      iconHome: 'ipad~home.png',
      iconCamera: 'ipad~camera.png',
      iconQuit: 'ipad~cancel.png',
      iconSettings: 'menu_icon.png',
    },
  };
})();
