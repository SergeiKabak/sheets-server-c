/* util.js — общие утилиты, без зависимостей */
(function (A) {
  'use strict';

  var RU_M = ['Январь', 'Февраль', 'Март', 'Апрель', 'Май', 'Июнь',
              'Июль', 'Август', 'Сентябрь', 'Октябрь', 'Ноябрь', 'Декабрь'];

  /* Экранирование для вставки в HTML-строку */
  function esc(s) {
    if (!s) return '';
    return String(s)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  function pad2(n) { return (n < 10 ? '0' : '') + n; }

  /* «Октябрь 2026» | «10.2026» -> {m:1..12, y:YYYY} | null */
  function parseMonthTitle(t) {
    if (!t) return null;
    var s = String(t).trim();
    var m = s.match(/^(\d{1,2})\.(\d{4})$/);
    if (m) return { m: parseInt(m[1], 10), y: parseInt(m[2], 10) };
    for (var i = 0; i < 12; i++) {
      if (s.indexOf(RU_M[i]) === 0) {
        var y = s.match(/(\d{4})/);
        return { m: i + 1, y: y ? parseInt(y[1], 10) : new Date().getFullYear() };
      }
    }
    return null;
  }

  /* «7:30-14:40» (и типографские тире) -> ['07:30','14:40'] */
  function shiftTimes(s) {
    if (!s) return ['', ''];
    var t = String(s).replace(/[–—−]/g, '-').replace(/\s/g, '');
    var m = t.match(/(\d{1,2}):(\d{2})-(\d{1,2}):(\d{2})/);
    if (!m) return ['', ''];
    return [pad2(parseInt(m[1], 10)) + ':' + m[2],
            pad2(parseInt(m[3], 10)) + ':' + m[4]];
  }

  /* Алерт Telegram WebApp, если доступен, иначе обычный alert не вызываем */
  function tgAlert(msg) {
    var tg = window.Telegram && window.Telegram.WebApp;
    if (tg && tg.showAlert) { try { tg.showAlert(msg); } catch (e) {} }
  }

  /* Поиск ближайшего предка с селектором (замена Element.closest
     для старых webview) */
  function up(node, sel) {
    if (node && node.nodeType === 3) node = node.parentNode;
    while (node && node.nodeType === 1) {
      if (node.matches ? node.matches(sel)
        : (node.msMatchesSelector && node.msMatchesSelector(sel))) return node;
      node = node.parentNode;
    }
    return null;
  }

  A.esc = esc;
  A.pad2 = pad2;
  A.parseMonthTitle = parseMonthTitle;
  A.shiftTimes = shiftTimes;
  A.tgAlert = tgAlert;
  A.up = up;
})(window.APP = window.APP || {});
