/* api.js — все HTTP-вызовы Mini App в одном месте (fetch, без XHR) */
(function (A) {
  'use strict';

  /* Возвращает промис с распарсенным JSON.
     Никогда не отклоняется: сетевые/форматные ошибки -> {ok:false,error} */
  function json(url, opts) {
    return fetch(url, opts).then(function (r) {
      return r.text().then(function (t) {
        try { return JSON.parse(t); }
        catch (e) { return { ok: false, error: 'HTTP ' + r.status }; }
      });
    }, function () {
      return { ok: false, error: 'Сеть недоступна' };
    });
  }

  function q(obj) {
    var parts = [];
    for (var k in obj) {
      if (obj[k] === undefined || obj[k] === null || obj[k] === '') continue;
      parts.push(encodeURIComponent(k) + '=' + encodeURIComponent(obj[k]));
    }
    return parts.length ? '?' + parts.join('&') : '';
  }

  A.api = {
    json: json,

    months: function () {
      return json('/api/months');
    },

    month: function (name) {
      return json('/api/month' + q({ name: name }));
    },

    colleagues: function (month, day, cabinet, shift) {
      return json('/api/cabinet' + q({ month: month, cabinet: cabinet, day: day, shift: shift }));
    },

    /* --- Time tracking --- */
    trackingDays: function (month) {
      return json('/api/tracking/days' + q({ month: month }));
    },

    clock: function (action) {
      return json('/api/tracking/clock' + q({ action: action }), { method: 'POST' });
    },

    erase: function (month, day) {
      return json('/api/tracking/erase' + q({ month: month, day: day }), { method: 'POST' });
    },

    fill: function (month, day, inTime, outTime) {
      return json('/api/tracking/fill' + q({
        month: month, day: day, 'in': inTime, out: outTime
      }), { method: 'POST' });
    }
  };
})(window.APP = window.APP || {});
