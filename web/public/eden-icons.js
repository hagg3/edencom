// eden-icons.js — the port's icon set, as inline SVG.
// Requires: nothing. Publishes: window.EdenIcons. See docs/ui.md's dependency graph (audit I2).
//
// WHY INLINE SVG AND NOT PNGs: the source Figma types its sidebar icons as literal Apple SF
// Symbols private-use characters ("喇" et al) in the "SF Pro" font — an iOS-only, closed-
// license dependency that cannot ship in a web build at all. The design system replaced them with
// the open, ISC-licensed Lucide visual language, and this file inlines the handful of paths the
// port actually uses so there is no CDN fetch, no sprite sheet, no bundle weight, and no CORS
// story. Icons inherit `currentColor`, which is what makes them invert for free when a button
// enters its pressed state (see eden-ui.css's press mechanic).
//
// THE ONLY RASTER ICONS IN THE PORT are the three main-menu home tiles and the wordmark — the
// engine's own pre-rendered 3D block art (media/ipad_menu/), loaded through eden-assets.js. Those
// are 3D renders with baked lighting and glow; redrawing them as SVG would lose the thing that
// makes the title screen look like Eden. Everything else is here.
//
// STROKE WEIGHT: Lucide's default is 2 at 24px. That reads as spindly next to this UI's 2-3px
// keylines and hard bevels, so the default here is 2.4 — heavy enough to sit in a 64px beveled
// tile without looking like a placeholder.
(function () {
  'use strict';

  // Path data is Lucide (https://lucide.dev, ISC). Multiple subpaths are joined with "|" — a
  // separator, not SVG syntax; render() splits on it into sibling <path> elements.
  var PATHS = {
    // --- settings groups (one per kSettings[] group, plus the two JS-owned tabs) -------------
    // The design-system kit shipped a mangled simplification of this one that renders as a bare
    // diagonal bar with no wrench head — this is Lucide's actual geometry.
    wrench: 'M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z',
    'volume-2': 'M11 5 6 9H2v6h4l5 4V5Z|M15.5 8.5a5 5 0 0 1 0 7|M18.5 5.5a9 9 0 0 1 0 13',
    'gamepad-2': 'M6 12h4|M8 10v4|M15 13h.01|M18 11h.01|M17.32 5H6.68a4 4 0 0 0-3.978 3.59c-.006.052-.01.101-.017.152C2.604 9.416 2 14.456 2 16a3 3 0 0 0 3 3c1 0 1.5-.5 2-1l1.414-1.414A2 2 0 0 1 9.828 16h4.344a2 2 0 0 1 1.414.586L17 18c.5.5 1 1 2 1a3 3 0 0 0 3-3c0-1.544-.604-6.584-.685-7.258-.007-.05-.011-.1-.017-.152A4 4 0 0 0 17.32 5Z',
    monitor: 'M20 3H4a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h16a1 1 0 0 0 1-1V4a1 1 0 0 0-1-1Z|M8 21h8|M12 17v4',
    hand: 'M18 11V6a2 2 0 0 0-2-2a2 2 0 0 0-2 2v3|M14 10V4a2 2 0 0 0-2-2a2 2 0 0 0-2 2v6|M10 10.5V6a2 2 0 0 0-2-2a2 2 0 0 0-2 2v10|M6 12v5a6 6 0 0 0 6 6h2a6 6 0 0 0 6-6v-6a2 2 0 0 0-2-2a2 2 0 0 0-2 2',
    'flask-conical': 'M10 2v6.29a2 2 0 0 1-.5 1.32L4.21 15.7a2 2 0 0 0 1.5 3.3h12.58a2 2 0 0 0 1.5-3.3l-5.3-6.1A2 2 0 0 1 14 8.3V2|M8.5 2h7|M7 15h10',
    'sliders-horizontal': 'M21 4h-7|M10 4H3|M21 12h-9|M8 12H3|M21 20h-5|M12 20H3|M14 2v4|M8 10v4|M16 18v4',
    settings: 'M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6Z|M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1Z',
    keyboard: 'M2 6a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2z|M6 8h.01|M10 8h.01|M14 8h.01|M18 8h.01|M8 12h.01|M12 12h.01|M16 12h.01|M7 16h10',
    'hard-drive': 'M22 12H2|M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z|M6 16h.01|M10 16h.01',

    // --- new-world generator types ------------------------------------------------------------
    square: 'M4 4h16v16H4z',
    // Also a kit simplification that read as two bare chevrons rather than a peak — Lucide's own.
    mountain: 'm8 3 4 8 5-5 5 15H2L8 3z',
    trees: 'M10 10v.2A3 3 0 0 1 8.9 16H5a3 3 0 0 1-1-5.8V10a3 3 0 0 1 6 0Z|M7 16v6|M13 19v3|M12 19h8.3a1 1 0 0 0 .7-1.7L18 14h.3a1 1 0 0 0 .7-1.7L16 9h.2a1 1 0 0 0 .8-1.7L13 3l-1.4 1.5',

    // --- actions -------------------------------------------------------------------------------
    play: 'M6 3l14 9-14 9V3z',
    'chevron-left': 'm15 18-6-6 6-6',
    'chevron-right': 'm9 18 6-6-6-6',
    x: 'M18 6 6 18|M6 6l12 12',
    'trash-2': 'M3 6h18|M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2|M10 11v6|M14 11v6',
    wifi: 'M12 20h.01|M2 8.82a15 15 0 0 1 20 0|M5 12.86a10 10 0 0 1 14 0|M8.5 16.43a5 5 0 0 1 7 0',
    info: 'M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20Z|M12 16v-4|M12 8h.01',
    'square-plus': 'M4 4h16v16H4z|M12 8v8|M8 12h8',
    'folder-open': 'm6 14 1.5-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2',
    download: 'M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4|M7 10l5 5 5-5|M12 15V3',
    save: 'M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z|M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7|M7 3v4a1 1 0 0 0 1 1h7',
    home: 'm3 9 9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z|M9 22V12h6v10',
    camera: 'M14.5 4h-5L7 7H4a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3l-2.5-3z|M12 17a4 4 0 1 0 0-8 4 4 0 0 0 0 8Z',
    'log-out': 'M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4|m16 17 5-5-5-5|M21 12H9',
    'rotate-ccw': 'M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8|M3 3v5h5',
    'alert-triangle': 'm21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3Z|M12 9v4|M12 17h.01',
  };

  // `play` is a solid triangle in the mockups, not an outline. Any other closed glyph that should
  // be filled rather than stroked goes here.
  var FILLED = { play: true };

  var NS = 'http://www.w3.org/2000/svg';

  /**
   * Build an <svg> element for `name`.
   *
   * The result is aria-hidden by default: icons in this UI always sit next to a text label or on
   * a control that already carries an aria-label, so announcing the icon too would just make
   * screen readers say everything twice. Pass `opts.title` for the rare standalone case.
   */
  function svg(name, opts) {
    opts = opts || {};
    var d = PATHS[name];
    var el = document.createElementNS(NS, 'svg');
    el.setAttribute('viewBox', '0 0 24 24');
    el.setAttribute('fill', 'none');
    el.setAttribute('class', 'eden-icon' + (opts.className ? ' ' + opts.className : ''));
    if (opts.size) {
      el.setAttribute('width', opts.size);
      el.setAttribute('height', opts.size);
    }
    if (opts.title) {
      el.setAttribute('role', 'img');
      var t = document.createElementNS(NS, 'title');
      t.textContent = opts.title;
      el.appendChild(t);
    } else {
      el.setAttribute('aria-hidden', 'true');
    }
    if (!d) {
      // An unknown name is a typo, and silently drawing nothing hides it until someone notices a
      // blank button. Draw a visible "missing glyph" box instead — cheap, and impossible to miss.
      var miss = document.createElementNS(NS, 'rect');
      miss.setAttribute('x', '3'); miss.setAttribute('y', '3');
      miss.setAttribute('width', '18'); miss.setAttribute('height', '18');
      miss.setAttribute('stroke', 'currentColor');
      miss.setAttribute('stroke-width', '2');
      miss.setAttribute('stroke-dasharray', '3 3');
      el.appendChild(miss);
      return el;
    }
    var filled = FILLED[name];
    if (!filled) {
      el.setAttribute('stroke', 'currentColor');
      el.setAttribute('stroke-width', String(opts.strokeWidth || 2.4));
      el.setAttribute('stroke-linecap', 'round');
      el.setAttribute('stroke-linejoin', 'round');
    }
    d.split('|').forEach(function (seg) {
      var p = document.createElementNS(NS, 'path');
      p.setAttribute('d', seg);
      if (filled) p.setAttribute('fill', 'currentColor');
      el.appendChild(p);
    });
    return el;
  }

  function has(name) { return Object.prototype.hasOwnProperty.call(PATHS, name); }

  window.EdenIcons = {
    svg: svg,
    has: has,
    names: function () { return Object.keys(PATHS); },
  };
})();
