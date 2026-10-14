// Run in a Twitch tab's DevTools console, or paste the expression directly.
// A true result verifies Chromium advertises the exact MP4 codecs Twitch needs.
(() => {
  const mime = 'video/mp4; codecs="avc1.42E01E, mp4a.40.2"';
  const supported = typeof MediaSource !== 'undefined' &&
                    MediaSource.isTypeSupported(mime);
  console.table({ mime, supported, userAgent: navigator.userAgent });
  return supported;
})();
