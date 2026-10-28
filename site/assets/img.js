// Inlined at the end of <body>. Picks an image tier from the connection, loads
// images as they near the viewport, and runs the lightbox.
(function () {
  var d = document, RANK = { dither: 0, lite: 1, full: 2 };

  // The lite and dither tiers are WebP-only; without WebP everything gets
  // the full tier, whose <picture> falls back to JPEG.
  var webp = d.createElement("canvas").toDataURL("image/webp").indexOf("data:image/webp") === 0;

  function tier() {
    var c = navigator.connection;
    if (!c || !webp) return "full";
    if (c.saveData || /2g/.test(c.effectiveType)) return "dither";
    if (c.effectiveType === "3g") return "lite";
    return "full";
  }

  function parse(srcset) {
    return srcset.split(", ").map(function (s) {
      var p = s.split(" ");
      return { url: p[0], w: parseInt(p[1], 10) };
    });
  }

  function load(img, t) {
    var cur = img.getAttribute("data-tier");
    if (cur && RANK[cur] >= RANK[t]) return;
    img.setAttribute("data-tier", t);
    img.addEventListener("load", function () { img.setAttribute("data-loaded", ""); }, { once: true });
    var sources = img.parentNode.querySelectorAll("source");
    if (t === "full") {
      sources.forEach(function (s) { s.srcset = s.getAttribute("data-srcset"); });
      img.srcset = img.getAttribute("data-full");
      removeUpgrade(img);
      return;
    }
    sources.forEach(function (s) { s.removeAttribute("srcset"); });
    var list = parse(img.getAttribute("data-" + t));
    if (t === "dither") {
      // Ignore device pixel ratio: the smallest variant at least as wide as
      // the layout box, shown as crisp scaled-up pixels.
      var want = img.clientWidth || 400, pick = list[list.length - 1];
      for (var i = 0; i < list.length; i++) if (list[i].w >= want) { pick = list[i]; break; }
      img.removeAttribute("srcset");
      img.src = pick.url;
    } else {
      img.srcset = img.getAttribute("data-lite");
    }
    addUpgrade(img);
  }

  function holder(img) {
    var wrap = img.closest(".zoom, .img-wrap");
    return wrap.parentNode.tagName === "FIGURE" ? wrap.parentNode : wrap;
  }

  function addUpgrade(img) {
    if (img._upgrade) return;
    var b = d.createElement("button");
    b.type = "button";
    b.className = "upgrade";
    b.textContent = "Load full-quality image";
    b.addEventListener("click", function () { load(img, "full"); });
    var h = holder(img);
    if (h.tagName === "FIGURE") h.insertBefore(b, h.children[1] || null);
    else h.after(b);
    img._upgrade = b;
  }

  function removeUpgrade(img) {
    if (img._upgrade) { img._upgrade.remove(); img._upgrade = null; }
  }

  var imgs = d.querySelectorAll("img[data-full]");
  var io = "IntersectionObserver" in window && new IntersectionObserver(function (entries) {
    entries.forEach(function (e) {
      if (e.isIntersecting) { io.unobserve(e.target); load(e.target, tier()); }
    });
  }, { rootMargin: "200px 0px" });
  imgs.forEach(function (img) {
    if (!io || img.hasAttribute("data-eager")) {
      img.fetchPriority = "high";
      load(img, tier());
    } else {
      io.observe(img);
    }
  });

  // Lightbox: always the original-size image, whatever the tier.
  var dlg, opener;
  function lightbox(a) {
    var img = a.querySelector(".img-js img");
    if (!dlg) {
      dlg = d.createElement("dialog");
      dlg.className = "lightbox";
      dlg.innerHTML = '<button type="button" class="close">Close</button>' +
        '<picture><source type="image/avif"><source type="image/webp"><img alt=""></picture>';
      dlg.querySelector(".close").addEventListener("click", function () { dlg.close(); });
      dlg.addEventListener("click", function (e) {
        if (e.target === dlg || e.target.tagName === "PICTURE") dlg.close();
      });
      dlg.addEventListener("close", function () { opener && opener.focus(); });
      d.body.appendChild(dlg);
    }
    var s = dlg.querySelectorAll("source"), big = dlg.querySelector("img");
    big.removeAttribute("src");
    // Show what is already on screen until the original arrives.
    big.style.backgroundImage = 'url("' + (img.currentSrc || img.src) + '")';
    s[0].srcset = a.getAttribute("data-avif");
    s[1].srcset = a.getAttribute("data-webp");
    big.width = +a.getAttribute("data-width");
    big.height = +a.getAttribute("data-height");
    big.alt = img.alt;
    big.src = a.href;
    dlg.setAttribute("aria-label", "Image: " + img.alt);
    opener = a;
    dlg.showModal();
  }

  d.addEventListener("click", function (e) {
    var a = e.target.closest && e.target.closest("a.zoom");
    if (!a || e.button || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
    if (typeof HTMLDialogElement !== "function") return;
    e.preventDefault();
    lightbox(a);
  });
})();
