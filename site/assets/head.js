// Inlined in <head>: runs before first paint. DOCS, the docs URLs in sidebar order, is
// prepended by the build.
document.documentElement.className = "js";
// An explicit theme choice (from the toggle) overrides the system setting.
try {
  var theme = localStorage.getItem("theme");
  if (theme) document.documentElement.setAttribute("data-theme", theme);
} catch (e) {}

// Cross-document view transitions (style.css has the animations). Home to docs slides
// left and back slides right; between docs pages the content slides up to a page lower
// in the sidebar and down to one above it. Both pages add the same type: the old one
// so its snapshot is taken with the right view-transition-names, the new one to animate.
(function () {
  function path(url) {
    try { return new URL(url).pathname; } catch (e) { return ""; }
  }
  function isDocs(p) { return p.indexOf("/docs/") === 0; }
  function type(from, to) {
    if (from === "/" && isDocs(to)) return "to-docs";
    if (isDocs(from) && to === "/") return "to-home";
    var i = DOCS.indexOf(from), j = DOCS.indexOf(to);
    if (i < 0 || j < 0 || i === j) return "";
    return j > i ? "docs-next" : "docs-prev";
  }
  function apply(vt, t) {
    if (t) vt.types.add(t);
    else vt.skipTransition();
  }
  addEventListener("pageswap", function (e) {
    if (e.viewTransition && e.activation) apply(e.viewTransition, type(location.pathname, path(e.activation.entry.url)));
  });
  addEventListener("pagereveal", function (e) {
    if (!e.viewTransition) return;
    var a = window.navigation && navigation.activation;
    apply(e.viewTransition, a && a.from ? type(path(a.from.url), location.pathname) : "");
  });
})();
