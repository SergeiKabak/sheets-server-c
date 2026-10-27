/* tracking.js — учёт рабочего времени: загрузка отметок, ввод времени,
   кнопки «Пришла/Ушла», кнопка «Деньги», статус и баннеры */
(function (A) {
  'use strict';

  var S = A.state;

  /* ---------- баннеры ---------- */
  function trkMsg(txt, ok) {
    var el = document.getElementById('trk-st');
    if (!el) return;
    el.textContent = txt;
    el.className = ok ? 'trk-banner ok' : 'trk-banner err';
    el.style.display = 'block';
  }

  function trkStatus() {
    A.api.trackingSummary().then(function (dd) {
      if (!dd || !dd.ok) return;
      trkMsg('Отработано за ' + (dd.month || '') + ': ' +
             dd.total_hours + ' ч., сумма ' + dd.total_sum, true);
    });
  }

  /* ---------- данные отметок ---------- */
  function loadTracking(month) {
    if (!month) return;
    A.api.trackingDays(month).then(function (d) {
      /* устаревший ответ — пользователь уже переключил месяц */
      if (!d || !d.ok || S.trkSelectedMonth !== month) return;
      S.TRK = d.days || {};
      S.TRK_META = { month: d.month || '', label: d.label || month, today: d.today || '' };
      A.calendar.renderTracking();
    });
  }

  /* Сумма заработка за месяц по загруженным отметкам */
  function monthSum() {
    var s = 0, TRK = S.TRK;
    for (var k in TRK) {
      var r = TRK[k];
      if (r && r.sum) {
        var v = parseFloat(r.sum);
        if (isFinite(v)) s += v;
      }
    }
    return s;
  }

  /* ---------- тап по дню ---------- */
  function dayAction(day) {
    if (!S.CAL_HAS[day]) return;
    var rec = S.TRK[day] || null;
    if (rec && rec.in && rec.out) {
      var t = 'День ' + day + ' (' + (S.TRK_META.label || S.trkSelectedMonth || '') + '):\n' +
              'Приход: ' + rec.in + '\nУход: ' + rec.out;
      if (rec.hours) t += '\nОтработано: ' + rec.hours + ' ч.';
      if (rec.sum) t += '\nСумма: ' + rec.sum;
      alert(t);
      return;
    }
    openTrkDialog(day, '', '');
  }

  /* ---------- модалка ввода времени ---------- */
  function openTrkDialog(day, cabinet, shift) {
    if (!S.CAL_HAS[day]) { trkMsg('В этот день смен нет — время не вносится', false); return; }
    var ex = S.TRK[day] || null;
    if (ex && ex.in && ex.out) {
      trkMsg('За день ' + day + ' время уже отмечено: ' + ex.in + '–' + ex.out, false);
      return;
    }
    S.trkPendingDay = day;
    S.trkPendingMonth = S.M[S.cur] || S.TRK_META.label || '';

    var rec = S.TRK[day] || {};
    var st = A.shiftTimes(shift);
    var i = document.getElementById('trk-in-time'), o = document.getElementById('trk-out-time');
    if (i) i.value = rec.in || st[0] || '';
    if (o) o.value = rec.out || st[1] || '';

    var t = document.getElementById('trk-modal-title');
    if (t) t.textContent = (cabinet ? 'Кабинет ' + cabinet + ', ' : '') +
                           'день ' + day + ' (' + S.trkPendingMonth + ')';
    var e = document.getElementById('trk-modal-err');
    if (e) e.textContent = '';
    var m = document.getElementById('trk-modal');
    if (m) m.classList.add('open');
  }

  function closeTrkDialog() {
    var m = document.getElementById('trk-modal');
    if (m) m.classList.remove('open');
    var e = document.getElementById('trk-modal-err');
    if (e) e.textContent = '';
    S.trkPendingDay = 0;
    S.trkPendingMonth = '';
  }

  function saveTrkDialog() {
    var i = document.getElementById('trk-in-time'), o = document.getElementById('trk-out-time');
    var e = document.getElementById('trk-modal-err'), b = document.getElementById('trk-modal-save');
    var iv = i ? i.value : '', ov = o ? o.value : '';
    if (!iv || !ov) { if (e) e.textContent = 'Укажите время прихода и ухода.'; return; }
    if (!S.trkPendingDay || !S.trkPendingMonth) { if (e) e.textContent = 'Не выбран день.'; return; }

    var day = S.trkPendingDay;
    if (b) b.disabled = true;
    if (e) e.textContent = '';

    A.api.fill(S.trkPendingMonth, day, iv, ov).then(function (d) {
      if (d && d.ok) {
        S.TRK[day] = { in: d.in, out: d.out, hours: d.hours, sum: d.sum };
        A.calendar.renderTracking();
        closeTrkDialog();
        trkMsg('Записано: ' + d.date + ', ' + d.hours + ' ч., ' + d.sum, true);
        trkStatus();
      } else if (e) {
        e.textContent = (d && d.error) ? d.error : 'Не удалось сохранить';
      }
      if (b) b.disabled = false;
    });
  }

  /* ---------- отметка прихода/ухода ---------- */
  function clock(action, cb) {
    A.api.clock(action).then(function (r) {
      if (r && r.ok) {
        var msg = 'Записано! ' + (action === 'in' ? 'Приход ' : 'Уход ') + r.time;
        if (r.hours !== undefined && r.hours !== null) msg += '\nОтработано: ' + r.hours + ' ч.';
        if (r.sum !== undefined && r.sum !== null && r.sum !== '') msg += '\nСумма: ' + r.sum;
        trkMsg(msg, true);
        A.tgAlert(msg);
        loadTracking(S.trkSelectedMonth);
      } else {
        var err = (r && r.error) || 'Ошибка запроса';
        trkMsg('Ошибка: ' + err, false);
        A.tgAlert(err);
      }
      if (cb) cb(r || { ok: false });
    });
  }

  /* ---------- кнопка «Деньги»: суммы по дням в календаре ---------- */
  function moneyToggle() {
    S.MONEY_VIEW = !S.MONEY_VIEW;
    A.calendar.updateMoneyInfo();
    if (S.MONEY_VIEW) {
      var s = monthSum();
      trkMsg(s > 0
        ? ('Заработано за ' + (S.TRK_META.label || S.trkSelectedMonth || ' месяц') +
           ': ' + s.toFixed(2) + '. Суммы по дням — в календаре.')
        : 'За этот месяц пока нет отработанных дней', s > 0);
    } else {
      trkStatus();
    }
  }

  /* ---------- привязка UI (один раз при старте) ---------- */
  function bindUI() {
    var b1 = document.getElementById('trk-in'),
        b2 = document.getElementById('trk-out'),
        bm = document.getElementById('trk-money'),
        sv = document.getElementById('trk-modal-save'),
        cn = document.getElementById('trk-modal-cancel'),
        md = document.getElementById('trk-modal');

    if (sv) sv.onclick = saveTrkDialog;
    if (cn) cn.onclick = closeTrkDialog;
    if (md) md.onclick = function (e) { if (e.target === md) closeTrkDialog(); };
    if (b1) b1.onclick = function () {
      b1.disabled = true; trkMsg('Сохраняем...', true);
      clock('in', function () { b1.disabled = false; trkStatus(); });
    };
    if (b2) b2.onclick = function () {
      b2.disabled = true; trkMsg('Сохраняем...', true);
      clock('out', function () { b2.disabled = false; trkStatus(); });
    };
    if (bm) bm.onclick = moneyToggle;

    trkStatus();
  }

  A.tracking = {
    trkMsg: trkMsg,
    trkStatus: trkStatus,
    loadTracking: loadTracking,
    monthSum: monthSum,
    dayAction: dayAction,
    openTrkDialog: openTrkDialog,
    closeTrkDialog: closeTrkDialog,
    saveTrkDialog: saveTrkDialog,
    clock: clock,
    moneyToggle: moneyToggle,
    bindUI: bindUI
  };
})(window.APP = window.APP || {});
