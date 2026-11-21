// Inlined on the home page: makes the download for the visitor's platform the main
// button and moves it first. Without JS, or on other platforms, all of them show alike.
(function () {
  var list = document.querySelector(".downloads");
  if (!list) return;
  var ua = navigator.userAgent;
  var p = (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || "";
  // Android before Linux: Android's user agent says Linux too. ChromeOS runs Android apps.
  var os = /Android|CrOS|Chrome OS/i.test(ua + " " + p) ? "android"
    : /^Win/i.test(p) || /Windows/.test(ua) ? "windows"
    : /Linux|X11/i.test(p + " " + ua) ? "linux" : "";
  var a = os && list.querySelector('[data-platform="' + os + '"]');
  if (!a) return;
  a.classList.add("is-primary");
  list.prepend(a.parentNode);
  list.classList.add("has-primary");
})();
