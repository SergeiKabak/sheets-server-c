/* state.js — единственное общее состояние Mini App */
(function (A) {
  'use strict';

  A.state = {
    /* месяцы и выбранный индекс */
    M: [],
    cur: -1,

    /* отметки Time tracking: TRK[день] = {in,out,hours,sum} */
    TRK: {},
    TRK_META: { month: '', label: '', today: '', rate: '' },  /* month: «MM.YYYY» */
    trkSelectedMonth: '',

    /* модалка ввода времени */
    trkPendingDay: 0,
    trkPendingMonth: '',

    /* картa «в этом дне есть смены» и режим показа сумм */
    CAL_HAS: {},
    MONEY_VIEW: false
  };
})(window.APP = window.APP || {});
