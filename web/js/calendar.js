/* calendar.js — построение календаря месяца и отметок Time tracking.
   Один раз строим HTML, один раз индексируем ячейки (кэш DOM-ссылок),
   клик обрабатывается одним делегированным слушателем. */
(function (A) {
  'use strict';

  var S = A.state, esc = A.esc;
  var WD = ['Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб', 'Вс'];

  /* Кэш после построения: чтобы renderTracking() не делал
     querySelector по 2 раза на каждую из 31 ячейки при каждом рендере */
  var dayCells = [];          /* [{day, has, cell, mark, sum}] */
  var elTable = null, elMoney = null, elCal = null, elBtnMoney = null;

  function tableEl() { return elTable || (elTable = document.getElementById('table')); }

  function resetElCache() {
    elTable = document.getElementById('table');
    elMoney = null; elCal = null; elBtnMoney = null;
  }

  /* Индексирует ячейки дней один раз после innerHTML */
  function indexDayCells(root) {
    var nodes = root.querySelectorAll('.cal-d[data-day]');
    dayCells = new Array(nodes.length);
    for (var i = 0; i < nodes.length; i++) {
      var cell = nodes[i];
      dayCells[i] = {
        day: parseInt(cell.getAttribute('data-day'), 10),
        has: cell.getAttribute('data-has') === '1',
        cell: cell,
        mark: cell.querySelector('.cal-mark'),
        sum: cell.querySelector('.cal-sum')
      };
    }
  }

  /* «Сегодня» (число) в выбранном месяце, иначе 0 */
  function todayDay() {
    var meta = S.TRK_META;
    if (!meta.today || !meta.month) return 0;
    var tp = String(meta.today).split('.'), mp = String(meta.month).split('.');
    if (tp.length !== 3 || mp.length !== 2) return 0;
    if (parseInt(mp[0], 10) !== parseInt(tp[1], 10)) return 0;
    if (parseInt(mp[1], 10) !== parseInt(tp[2], 10)) return 0;
    return parseInt(tp[0], 10) || 0;
  }

  /* Главная отрисовка месяца: инфоблок + сетка Пн-Вс */
  function showTable(d) {
    var w = tableEl();
    if (!w) return;

    var s1 = d.s1 || [], s2 = d.s2 || [];
    var n = Math.max(s1.length, s2.length, 1);
    var days = n > 3 ? n - 3 : 0;
    var sh1 = (s1[1] || '').trim(), sh2 = (s2[1] || '').trim();
    var total = s1[n - 1] || s2[n - 1] || '';

    var pm = A.parseMonthTitle(d.t);
    var dim = days;
    if (pm) { var md = new Date(pm.y, pm.m, 0).getDate(); if (md > dim) dim = md; }
    var lead = pm ? (new Date(pm.y, pm.m - 1, 1).getDay() + 6) % 7 : 0;

    var has = {};
    var p = [];

    p.push('<div class="cal-info"><span class="cal-title">', esc(d.t || ''), '</span>');
    if (sh1) p.push('<span class="cal-legend">Смена 1: <b>', esc(sh1), '</b></span>');
    if (sh2) p.push('<span class="cal-legend">Смена 2: <b>', esc(sh2), '</b></span>');
    if (total) p.push('<span class="cal-legend">Всего смен: <b>', esc(total), '</b></span>');
    p.push('<span class="cal-legend" id="cal-money"></span></div>');

    p.push('<div class="cal">');
    for (var i = 0; i < 7; i++) p.push('<div class="cal-h', (i > 4 ? ' we' : ''), '">', WD[i], '</div>');
    for (i = 0; i < lead; i++) p.push('<div class="cal-d pad"></div>');

    for (var day = 1; day <= dim; day++) {
      var c1 = (s1[day + 1] || '').trim();
      var c2 = (s2[day + 1] || '').trim();
      var work = !!(c1 || c2);
      if (work) has[day] = 1;
      var we = pm ? ((lead + day - 1) % 7 > 4) : false;

      p.push('<div class="cal-d', we ? ' we' : '', (we && work) ? ' we-work' : '',
             '" data-day="', day, '"', work ? ' data-has="1"' : '', '>');
      p.push('<span class="cal-mark"></span>');
      p.push('<div class="cal-num">', day, '</div>');
      if (c1) p.push('<span class="cal-cab" data-day="', day, '" data-cabinet="', esc(c1),
                     '" data-shift="', esc(sh1), '" title="', esc(sh1), '">', esc(c1), '</span>');
      if (c2) p.push('<span class="cal-cab" data-day="', day, '" data-cabinet="', esc(c2),
                     '" data-shift="', esc(c2 === c1 ? '' : sh2), '" title="', esc(sh2), '">',
                     esc(c2), '</span>');
      p.push('<span class="cal-sum"></span></div>');
    }

    var tail = (lead + dim) % 7;
    if (tail) for (i = tail; i < 7; i++) p.push('<div class="cal-d pad"></div>');
    p.push('</div>');

    w.innerHTML = p.join('');
    w.classList.remove('h');
    resetElCache();
    indexDayCells(w);
    S.CAL_HAS = has;

    S.trkSelectedMonth = (d.t || S.M[S.cur] || '');
    var tk = d.trk;
    if (tk && tk.ok) {
      S.TRK = tk.days || {};
      S.TRK_META = { month: tk.month || '', label: tk.label || S.trkSelectedMonth, today: tk.today || '' };
      renderTracking();
    } else {
      S.TRK = {}; S.TRK_META = { month: '', label: '', today: '' };
      A.tracking.loadTracking(S.trkSelectedMonth);
    }
  }

  /* Обновляет отметки/суммы/подсветку по кэшу ячеек — без запросов к DOM */
  function renderTracking() {
    var td = todayDay();
    var TRK = S.TRK;
    for (var i = 0; i < dayCells.length; i++) {
      var e = dayCells[i];
      var rec = TRK[e.day] || null;
      var done = !!(rec && rec.in && rec.out);

      e.cell.classList.toggle('cal-done', done);
      e.cell.classList.toggle('cal-today', td === e.day);

      if (done) {
        e.mark.textContent = '✓';
        e.mark.title = 'Приход ' + rec.in + ', уход ' + rec.out +
                       (rec.hours ? ', ' + rec.hours + ' ч.' : '');
      } else if (e.mark.textContent) {
        e.mark.textContent = '';
        e.mark.title = '';
      }

      var sv = parseFloat(rec && rec.sum);
      var txt = (isFinite(sv) && sv > 0) ? sv.toFixed(2) : '';
      if (e.sum.textContent !== txt) e.sum.textContent = txt;

      e.cell.title = e.has
        ? (done ? 'Просмотр отработанного времени' : 'Тап — внести время за день')
        : 'Смен в этот день нет';
    }
    updateMoneyInfo();
  }

  /* «Заработано: X» в инфоблоке + режим показа сумм по дням */
  function updateMoneyInfo() {
    var el = elMoney || (elMoney = document.getElementById('cal-money'));
    if (el) {
      var s = A.tracking.monthSum();
      var html = s > 0 ? ('Заработано: <b>' + s.toFixed(2) + '</b>') : '';
      if (el.innerHTML !== html) el.innerHTML = html;
    }
    var cal = elCal || (elCal = document.querySelector('.cal'));
    if (cal) cal.classList.toggle('show-sum', !!S.MONEY_VIEW);
    var b = elBtnMoney || (elBtnMoney = document.getElementById('trk-money'));
    if (b) b.classList.toggle('on', !!S.MONEY_VIEW);
  }

  /* Кто работает в кабинете в этот день */
  function showColleagues(day, cabinet, shift) {
    if (!cabinet) return;
    var m = S.M[S.cur];
    A.api.colleagues(m, day, cabinet, shift).then(function (d) {
      var l = (d && d.colleagues) || [];
      var t = 'Кабинет ' + cabinet + ', день ' + day + (m ? ' (' + m + ')' : '') +
              (shift ? ', смена ' + shift : '');
      t += l.length
        ? '\n\nВместе с Бинкевич Татьяна Эдуардовна:\n' +
          l.map(function (c) { return '• ' + c.name + (c.shift ? ' — ' + c.shift : ''); }).join('\n')
        : '\n\nНикто другой не работает в этом кабинете в этот день.';
      alert(t);
    });
  }

  /* Один слушатель на весь контейнер вместо 2 обработчиков на ячейку */
  function onTableClick(ev) {
    var cab = A.up(ev.target, '.cal-cab');
    if (cab) {
      showColleagues(parseInt(cab.getAttribute('data-day'), 10),
                     cab.getAttribute('data-cabinet'),
                     cab.getAttribute('data-shift'));
      return;
    }
    var cell = A.up(ev.target, '.cal-d[data-day]');
    if (cell) A.tracking.dayAction(parseInt(cell.getAttribute('data-day'), 10));
  }

  function bind(root) {
    if (root) root.addEventListener('click', onTableClick, false);
  }

  A.calendar = {
    showTable: showTable,
    renderTracking: renderTracking,
    updateMoneyInfo: updateMoneyInfo,
    showColleagues: showColleagues,
    bind: bind
  };
})(window.APP = window.APP || {});
