/* PSCapture — page behaviour shared by index.html / index.it.html */
(function () {
  var doc = document.documentElement;
  doc.classList.remove('no-js');
  var it = doc.lang === 'it';
  var reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  /* nav border once the page scrolls */
  var nav = document.querySelector('.nav');
  function onScroll() { nav.classList.toggle('scrolled', window.scrollY > 8); }
  window.addEventListener('scroll', onScroll, { passive: true });
  onScroll();

  /* app mock: replay the real start-up log, then flip the pill to STREAMING */
  var log = document.querySelector('.app-log');
  var pill = document.querySelector('.pill');
  if (log && pill) {
    var lines = Array.prototype.slice.call(log.children);
    var live = function () {
      pill.classList.add('live');
      pill.lastChild.textContent = ' STREAMING  29.8 fps';
    };
    /* the HTML ships in the final state, so no-JS and reduced-motion visitors see it as-is */
    if (!reduce) {
      pill.classList.remove('live');
      pill.lastChild.textContent = ' AVVIO...';
      lines.forEach(function (l) { l.style.display = 'none'; });
      var i = 0;
      (function next() {
        if (i < lines.length) {
          lines[i].style.display = '';
          i++;
          setTimeout(next, i === 1 ? 500 : 420 + Math.random() * 380);
        } else {
          live();
        }
      })();
    }
  }

  /* copy buttons on code blocks (prompts and comments are not copied) */
  document.querySelectorAll('.codeblock').forEach(function (block) {
    var btn = document.createElement('button');
    btn.className = 'copy';
    btn.type = 'button';
    btn.textContent = it ? 'Copia' : 'Copy';
    btn.addEventListener('click', function () {
      var pre = block.querySelector('pre').cloneNode(true);
      pre.querySelectorAll('.p, .c').forEach(function (n) { n.remove(); });
      var text = pre.textContent.split('\n')
        .map(function (s) { return s.replace(/\s+$/, ''); })
        .filter(function (s) { return s.length; })
        .join('\n');
      navigator.clipboard.writeText(text).then(function () {
        btn.textContent = it ? 'Copiato ✓' : 'Copied ✓';
        btn.classList.add('done');
        setTimeout(function () {
          btn.textContent = it ? 'Copia' : 'Copy';
          btn.classList.remove('done');
        }, 1600);
      });
    });
    block.appendChild(btn);
  });

  /* troubleshooting filter */
  var q = document.getElementById('q');
  if (q) {
    var issues = document.querySelectorAll('.issue');
    var none = document.querySelector('.noresult');
    q.addEventListener('input', function () {
      var words = q.value.toLowerCase().trim().split(/\s+/).filter(Boolean);
      var shown = 0;
      issues.forEach(function (el) {
        var txt = el.textContent.toLowerCase();
        var ok = words.every(function (w) { return txt.indexOf(w) !== -1; });
        el.hidden = !ok;
        if (ok) shown++;
      });
      none.hidden = shown !== 0;
    });
  }

  /* live star count on the GitHub buttons */
  fetch('https://api.github.com/repos/fstat13/PSCapture')
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (d) {
      if (!d || typeof d.stargazers_count !== 'number') return;
      document.querySelectorAll('[data-stars]').forEach(function (el) {
        el.textContent = d.stargazers_count.toLocaleString(doc.lang);
        el.hidden = false;
      });
    })
    .catch(function () {});

  /* fade sections in as they scroll into view */
  if ('IntersectionObserver' in window && !reduce) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (e.isIntersecting) { e.target.classList.add('in'); io.unobserve(e.target); }
      });
    }, { rootMargin: '0px 0px -8% 0px' });
    document.querySelectorAll('.reveal').forEach(function (el) { io.observe(el); });
  } else {
    document.querySelectorAll('.reveal').forEach(function (el) { el.classList.add('in'); });
  }
})();
