// Inlined at the end of <body> on every page: the light/dark toggle, top right on the
// home page and at the foot of the docs sidebar.
(function () {
  var root = document.documentElement;
  var btn = document.querySelector(".theme-toggle");
  if (!btn) return;
  var system = matchMedia("(prefers-color-scheme: dark)");

  function isDark() {
    var t = root.getAttribute("data-theme");
    return t ? t === "dark" : system.matches;
  }
  function sync() { btn.setAttribute("aria-pressed", String(isDark())); }

  btn.addEventListener("click", function () {
    var next = isDark() ? "light" : "dark";
    // Choosing what the system already says goes back to following the system.
    var follow = (next === "dark") === system.matches;
    if (follow) root.removeAttribute("data-theme");
    else root.setAttribute("data-theme", next);
    try {
      if (follow) localStorage.removeItem("theme");
      else localStorage.setItem("theme", next);
    } catch (e) {}
    sync();
  });
  system.addEventListener("change", sync);
  btn.closest("[hidden]").removeAttribute("hidden");
  sync();
})();
