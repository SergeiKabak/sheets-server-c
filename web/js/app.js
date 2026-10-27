/* app.js — тема, кнопки месяцев, загрузка месяца, запуск Mini App */
(function (A) {
  'use strict';

  var S = A.state;
  var monthsBuilt = false;
  var reqSeq = 0;            /* защита от ответов «не того месяца» при быстрых переключениях */

  /* ---------- тема ---------- */

  var THEME_KEY = 'theme';
  var META_LIGHT = '#f5f5f5';
  var META_DARK = '#14161a';

  function tgApp() {
    return window.Telegram && window.Telegram.WebApp;
  }

  function savedTheme() {
    try {
      var t = localStorage.getItem(THEME_KEY);
      return (t === 'dark' || t === 'light') ? t : null;
    } catch (e) { return null; }
  }

  function systemTheme() {
    var tg = tgApp();
    if (tg && tg.colorScheme) return tg.colorScheme;
    return (window.matchMedia &&
            matchMedia('(prefers-color-scheme: dark)').matches) ? 'dark' : 'light';
  }

  /* Применяет тему: атрибут на <html> (CSS-переменные), иконку кнопки,
     цвет системной панели браузера и шапки Telegram. */
  function applyTheme(theme) {
    var t = (theme === 'dark') ? 'dark' : 'light';
    document.documentElement.setAttribute('data-theme', t);

    var btn = document.getElementById('theme-toggle');
    if (btn) {
      /* иконка показывает, что произойдёт по нажатию */
      btn.textContent = (t === 'dark') ? '☀️' : '🌙';
      btn.setAttribute('aria-label',
        (t === 'dark') ? 'Включить светлую тему' : 'Включить тёмную тему');
      btn.setAttribute('title', btn.getAttribute('aria-label'));
      btn.setAttribute('aria-pressed', t === 'dark' ? 'true' : 'false');
    }

    var meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.setAttribute('content', (t === 'dark') ? META_DARK : META_LIGHT);

    var tg = tgApp();
    if (tg) {
      try {
        if (tg.setHeaderColor) tg.setHeaderColor((t === 'dark') ? META_DARK : META_LIGHT);
        if (tg.setBackgroundColor) tg.setBackgroundColor((t === 'dark') ? META_DARK : META_LIGHT);
      } catch (e) {}
    }
    return t;
  }

  function toggleTheme() {
    var next = (document.documentElement.getAttribute('data-theme') === 'dark')
      ? 'light' : 'dark';
    try { localStorage.setItem(THEME_KEY, next); } catch (e) {}
    applyTheme(next);
  }

  /* Тема на старте: сохранённый выбор важнее темы Telegram; если выбора нет,
     следуем за Telegram и его сменой темы в рантайме. */
  function initTheme() {
    applyTheme(savedTheme() || systemTheme());

    var btn = document.getElementById('theme-toggle');
    if (btn) btn.addEventListener('click', toggleTheme);

    var tg = tgApp();
    if (tg && tg.onEvent) {
      try {
        tg.onEvent('themeChanged', function () {
          if (!savedTheme()) applyTheme(systemTheme());
        });
      } catch (e) {}
    }
  }

  /* Кнопки месяцев строятся один раз, дальше только переключается .active;
     клик — одним делегированным обработчиком */
  function showMonths() {
    var c = document.getElementById('months');
    if (!c) return;
    if (!monthsBuilt || c.childElementCount !== S.M.length) {
      c.innerHTML = '';
      for (var i = 0; i < S.M.length; i++) {
        var b = document.createElement('button');
        b.className = 'b';
        b.type = 'button';
        b.setAttribute('data-i', i);
        b.textContent = S.M[i];
        c.appendChild(b);
      }
      monthsBuilt = true;
      c.onclick = function (ev) {
        var t = A.up(ev.target, 'button.b');
        if (!t) return;
        var idx = parseInt(t.getAttribute('data-i'), 10);
        if (!isNaN(idx)) load(idx);
      };
    }
    var kids = c.children;
    for (var j = 0; j < kids.length; j++) kids[j].classList.toggle('active', j === S.cur);
  }

  var MONTH_NAMES = ['Январь','Февраль','Март','Апрель','Май','Июнь',
                      'Июль','Август','Сентябрь','Октябрь','Ноябрь','Декабрь'];

  /* Подпись текущего месяца в формате списка («Октябрь 2026»). */
  function currentMonthLabel() {
    var d = new Date();
    return MONTH_NAMES[d.getMonth()] + ' ' + d.getFullYear();
  }

  /* Текущий месяц по умолчанию; если его нет в списке — первый. */
  function defaultIndex() {
    var cur = currentMonthLabel();
    for (var i = 0; i < S.M.length; i++)
      if (S.M[i] === cur) return i;
    return Math.max(0, S.M.length - 1);
  }

  function load(i) {
    if (i < 0 || i >= S.M.length) return;
    S.cur = i;
    showMonths();
    var seq = ++reqSeq;
    A.api.month(S.M[i]).then(function (d) {
      if (seq !== reqSeq) return;                /* устаревший ответ не рисуем */
      if (!d || !d.s1) { alert((d && d.error) || ('Нет данных: ' + S.M[i])); return; }
      A.calendar.showTable(d);
    });
  }

  function init() {
    initTheme();
    try {
      A.tracking.bindUI();
      A.calendar.bind(document.getElementById('table'));
    } catch (err) {
      var el = document.getElementById('trk-st');
      if (el) el.textContent = 'Ошибка: ' + err.message;
    }
    A.api.months().then(function (d) {
      S.M = (d && d.months) || [];
      if (S.M.length) load(defaultIndex());
      else alert('Нет данных');
    });
  }

  window.init = init;
  A.app = { init: init, load: load, showMonths: showMonths,
            applyTheme: applyTheme, toggleTheme: toggleTheme, initTheme: initTheme };
})(window.APP = window.APP || {});
