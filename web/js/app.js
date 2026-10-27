/* app.js — тема, кнопки месяцев, загрузка месяца, запуск Mini App */
(function (A) {
  'use strict';

  var S = A.state;
  var monthsBuilt = false;
  var reqSeq = 0;            /* защита от ответов «не того месяца» при быстрых переключениях */

  function applyTheme() {
    var s = 'light';
    var tg = window.Telegram && window.Telegram.WebApp;
    if (tg && tg.colorScheme) s = tg.colorScheme;
    document.body.setAttribute('data-theme', s);
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
    applyTheme();
    try {
      A.tracking.bindUI();
      A.calendar.bind(document.getElementById('table'));
    } catch (err) {
      var el = document.getElementById('trk-st');
      if (el) el.textContent = 'Ошибка: ' + err.message;
    }
    A.api.months().then(function (d) {
      S.M = (d && d.months) || [];
      if (S.M.length) load(S.M.length - 1);
      else alert('Нет данных');
    });
  }

  window.init = init;
  A.app = { init: init, load: load, showMonths: showMonths, applyTheme: applyTheme };
})(window.APP = window.APP || {});
