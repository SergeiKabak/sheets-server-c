/* Тесты Mini App (web/): календарь, отметки времени, заработок, коллеги-модалка.
   Запуск:  npm i --no-save jsdom && node tests/test_miniapp.js
   Работает без сборки — просто читает web/index.html и web/js/*.js. */
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const read = p => fs.readFileSync(path.join(ROOT, p), 'utf8');
const exists = p => fs.existsSync(path.join(ROOT, p));

let JSDOM;
try { ({ JSDOM } = require('jsdom')); }
catch (e) {
  console.error('Требуется jsdom: npm i --no-save jsdom');
  process.exit(2);
}

const HTML = read('web/index.html');
const JS_FILES = ['util', 'state', 'api', 'calendar', 'tracking', 'app']
  .map(f => 'web/js/' + f + '.js');

let fails = 0;
function ok(cond, msg) {
  if (cond) console.log('OK   ' + msg);
  else { fails++; console.log('FAIL ' + msg); }
}
const tick = () => new Promise(r => setTimeout(r, 0));

async function main() {
  // --- подключённые ассеты и модули существуют ---
  for (const m of HTML.matchAll(/(?:src|href)="([^"]+)"/g)) {
    const p = m[1];
    if (p.startsWith('/')) ok(exists('web' + p), 'asset exists: web' + p);
  }
  for (const f of JS_FILES) ok(exists(f), 'module exists: ' + f);
  ok(/id="colleagues-modal"/.test(HTML) && /id="colleagues-list"/.test(HTML),
     'модалка коллег размечена в index.html');

  const dom = new JSDOM(HTML, { runScripts: 'outside-only', url: 'http://localhost/' });
  const { window } = dom;
  const document = window.document;

  window.__fetches = [];
  window.__alerts = [];
  window.fetch = (u, o) => {
    const url = String(u);
    window.__fetches.push(url + (o && o.method ? ' [' + o.method + ']' : ''));
    if (url.indexOf('/api/tracking/erase') >= 0) {
      return Promise.resolve({
        status: 200,
        text: () => Promise.resolve(JSON.stringify({ ok: true }))
      });
    }
    if (url.indexOf('/api/cabinet') >= 0) {
      // мок сервера: фильтрация коллег по пересечению смен (как в find_cabinet_colleagues)
      const all = [{ name: 'Яновская Юлия Алексеевна', shift: '8:00-14:30' },
                   { name: 'Гринь Юлия Геннадьевна', shift: '15:10-21:00' }];
      const sm = url.match(/shift=([^&]*)/);
      const shift = sm ? decodeURIComponent(sm[1]) : '';
      const mins = s => { const m = /(\d{1,2}):(\d{2})-(\d{1,2}):(\d{2})/.exec(s);
                          return m ? [+m[1] * 60 + +m[2], +m[3] * 60 + +m[4]] : null; };
      let list = all;
      if (shift) {
        const q = mins(shift);
        list = all.filter(c => { const t = mins(c.shift);
                                 return q && t && q[0] < t[1] && t[0] < q[1]; });
      }
      return Promise.resolve({
        status: 200,
        text: () => Promise.resolve(JSON.stringify({ colleagues: list }))
      });
    }
    return new Promise(() => {});   // остальные запросы — висят
  };
  window.alert = t => window.__alerts.push(String(t));

  // eval модулей в том же порядке, что и <script defer> в index.html
  for (const f of JS_FILES) window.eval(read(f));
  const APP = window.APP;

  ok(typeof APP === 'object', 'namespace APP создана');
  ok(typeof APP.esc === 'function', 'util.js: утилиты на месте');
  ok(typeof APP.state === 'object' && typeof APP.state.TRK === 'object', 'state.js: состояние на месте');
  ok(typeof APP.api === 'object' && typeof APP.api.month === 'function', 'api.js: HTTP-слой на месте');
  ok(typeof APP.calendar === 'object', 'calendar.js: на месте');
  ok(typeof APP.tracking === 'object', 'tracking.js: на месте');
  ok(typeof APP.app === 'object' && typeof window.init === 'function', 'app.js: init() определён');
  ok(APP.tracking.trkStatus === undefined && APP.api.trackingSummary === undefined,
     'устаревший баннер «Отработано за …» полностью убран (trkStatus/trackingSummary нет)');

  // ручной init без загрузки месяцев
  APP.tracking.bindUI();
  APP.calendar.bind(document.getElementById('table'));

  const st = document.getElementById('trk-st');
  ok(st.style.display !== 'block' && st.textContent === '',
     'баннер статуса при старте не показывается');

  // как в реальном приложении: список месяцев уже загружен
  APP.state.M = ['Сентябрь 2026', 'Октябрь 2026'];
  APP.state.cur = 1;

  // --- фикстура: Октябрь 2026 (1 окт — чт, выходные 3,4,10,...) ---
  const days = 31;
  const s1 = ['Бинкевич Татьяна Эдуардовна', '7:30-14:40'];
  const s2 = ['', '15:10-21:00'];
  for (let d = 1; d <= days; d++) { s1.push(''); s2.push(''); }
  const setCab = (arr, day, cab) => { arr[day + 1] = cab; };
  setCab(s1, 1, 'K2.1'); setCab(s2, 1, 'K2.1');
  setCab(s1, 2, 'Ц');
  setCab(s1, 3, 'K4');      // сб — рабочая
  setCab(s1, 4, 'K1.2');    // вс — рабочая
  setCab(s2, 5, 'K2.2');
  setCab(s1, 16, 'K4');     // сб
  s1.push('6'); s2.push('');

  APP.calendar.showTable({
    t: 'Октябрь 2026', s1, s2,
    trk: { ok: true, month: '10.2026', label: 'Октябрь 2026', today: '05.10.2026',
           days: { 2: { in: '07:30', out: '14:40', hours: '6.5', sum: '650.00' } } }
  });

  const click = el => el.dispatchEvent(new window.MouseEvent('click', { bubbles: true }));
  const modal = document.getElementById('trk-modal');
  const colModal = document.getElementById('colleagues-modal');
  const cal = document.querySelector('.cal');
  const cells = [...document.querySelectorAll('.cal-d[data-day]')];
  const cell = d => cells.find(c => c.dataset.day === String(d));

  // --- структура календаря ---
  ok(!!cal, 'календарь отрисован');
  const heads = document.querySelectorAll('.cal-h');
  ok(heads.length === 7 && heads[5].textContent === 'Сб' && heads[5].classList.contains('we'),
     'заголовки Пн-Вс, Сб/Вс выходные');
  ok(cells.length === 31, '31 ячейка дней, got ' + cells.length);
  const lead = [];
  for (const ch of cal.children) { if (ch.hasAttribute('data-day')) break; if (ch.classList.contains('pad')) lead.push(ch); }
  ok(lead.length === 3, '3 ведущих пустых ячейки (1 окт — чт), got ' + lead.length);
  const weDays = cells.filter(c => c.classList.contains('we')).map(c => +c.dataset.day);
  ok(JSON.stringify(weDays) === JSON.stringify([3,4,10,11,17,18,24,25,31]),
     'выходные: ' + weDays.join(','));
  ok(cell(3).classList.contains('we-work') && cell(4).classList.contains('we-work'),
     'рабочие выходные помечены');
  ok(!cell(6).hasAttribute('data-has') && cell(1).getAttribute('data-has') === '1',
     'data-has только при наличии смены');
  ok(APP.state.CAL_HAS[1] === 1 && !APP.state.CAL_HAS[6], 'CAL_HAS в state корректна');
  ok(!!cell(2).querySelector('.cal-mark') && !!cell(2).querySelector('.cal-sum'),
     'элементы mark/sum созданы в разметке');
  ok(document.querySelectorAll('[onclick]').length === 0,
     'нет инлайн-атрибутов onclick (делегирование)');

  // --- тапы по ячейкам ---
  click(cell(1));
  ok(modal.classList.contains('open'), 'тап по дню 1 (смена, нет отметки) -> диалог времени');
  APP.tracking.closeTrkDialog();

  const alertsBefore = window.__alerts.length;
  click(cell(2));
  ok(modal.classList.contains('open'), 'отмеченный день: модалка открывается для ПРАВКИ');
  const inVal = document.getElementById('trk-in-time').value;
  const outVal = document.getElementById('trk-out-time').value;
  ok(inVal === '07:30' && outVal === '14:40', 'предзаполнены текущие значения: ' + inVal + '-' + outVal);
  const info = document.getElementById('trk-modal-info');
  ok(info.style.display === 'block' && /Сейчас отмечено: 07:30–14:40/.test(info.textContent),
     'подсказка о текущей отметке: ' + info.textContent);
  const eraseBtn = document.getElementById('trk-modal-erase');
  ok(eraseBtn.style.display === '', 'кнопка «Удалить время» видна для отмечённого дня');
  ok(window.__alerts.length === alertsBefore, 'алерт больше не вызывается');
  APP.tracking.closeTrkDialog();

  click(cell(6));
  ok(!modal.classList.contains('open') && window.__alerts.length === alertsBefore,
     'день без смены: тихо');

  // --- коллеги: модалка в стиле диалога времени (не alert) ---
  window.__fetches = [];
  const alertsSnap = window.__alerts.length;
  const cabs1 = cell(1).querySelectorAll('.cal-cab');
  ok(cabs1.length === 2 && cabs1[0].getAttribute('data-shift') === '7:30-14:40' &&
     cabs1[1].getAttribute('data-shift') === '15:10-21:00',
     'обе смены (даже при одинаковом кабинете) несут свой shift');
  click(cabs1[0]);
  ok(window.__fetches.length === 1 && /shift=7%3A30-14%3A40/.test(window.__fetches[0]),
     'тап по смене 1 -> фильтр shift=7:30-14:40: ' + decodeURIComponent(window.__fetches[0]));
  ok(!modal.classList.contains('open'), 'модалка времени не открывается');
  ok(colModal.classList.contains('open'), 'открывается модалка коллег (как диалог времени)');
  const title1 = document.getElementById('colleagues-title').textContent;
  ok(/Кабинет K2\.1/.test(title1) && /смена 7:30-14:40/.test(title1),
     'заголовок смены 1: ' + title1);
  await tick();
  const listHtml = document.getElementById('colleagues-list').innerHTML;
  ok(/Яновская Юлия Алексеевна/.test(listHtml) && !/Гринь Юлия Геннадьевна/.test(listHtml),
     'смена 1: только коллеги первой смены');
  ok(!/col-row/.test(listHtml) === false, 'строки списка в стиле диалога (.col-row)');
  ok(window.__alerts.length === alertsSnap, 'alert() не используется для коллег');

  document.getElementById('colleagues-close').click();
  ok(!colModal.classList.contains('open'), 'кнопка «Закрыть» закрывает модалку коллег');

  // теперь вторая смена — тот же кабинет, но свой фильтр
  window.__fetches = [];
  click(cell(1).querySelectorAll('.cal-cab')[1]);
  await tick();
  ok(window.__fetches.length === 1 && /shift=15%3A10-21%3A00/.test(window.__fetches[0]),
     'тап по смене 2 -> фильтр shift=15:10-21:00: ' + decodeURIComponent(window.__fetches[0]));
  const title2 = document.getElementById('colleagues-title').textContent;
  ok(/смена 15:10-21:00/.test(title2), 'заголовок смены 2: ' + title2);
  const list2 = document.getElementById('colleagues-list').innerHTML;
  ok(/Гринь Юлия Геннадьевна/.test(list2) && !/Нет коллег|Яновская/.test(list2),
     'смена 2: только коллеги второй смены');
  colModal.dispatchEvent(new window.MouseEvent('click', { bubbles: true }));  // клик по подложке
  ok(!colModal.classList.contains('open'), 'клик по подложке закрывает модалку коллег');

  // --- защита прямого вызова диалога времени ---
  APP.tracking.openTrkDialog(6, '', '');
  ok(!modal.classList.contains('open') && /смен нет/.test(st.textContent),
     'openTrkDialog отклоняет день без смены: ' + st.textContent);
  APP.tracking.openTrkDialog(6, '', '');
  ok(!modal.classList.contains('open') && /смен нет/.test(st.textContent),
     'повторный guard по дню без смены работает');

  // --- деньги ---
  const money = document.getElementById('cal-money');
  ok(!!money && /Заработано: <b>650\.00<\/b>/.test(money.innerHTML),
     'сумма месяца в инфоблоке: ' + (money && money.innerHTML));
  ok(cell(2).querySelector('.cal-sum').textContent === '650.00', 'сумма дня в ячейке');
  ok(cell(1).querySelector('.cal-sum').textContent === '', 'в ячейке без отметки суммы нет');
  ok(!cal.classList.contains('show-sum'), 'суммы по дням скрыты по умолчанию');

  click(document.getElementById('trk-money'));
  ok(cal.classList.contains('show-sum') && document.getElementById('trk-money').classList.contains('on'),
     'кнопка «Деньги»: суммы по дням + подсветка');
  ok(/650\.00/.test(st.textContent), 'баннер итога при включении: ' + st.textContent);
  click(document.getElementById('trk-money'));
  ok(!cal.classList.contains('show-sum'), 'повторный тап скрывает суммы');
  ok(st.style.display === 'none', 'при выключении баннер прячется (итог виден в инфоблоке)');

  // --- удаление отметки дня: открыть диалог правки и нажать «Удалить» ---
  click(cell(2));
  ok(modal.classList.contains('open') && eraseBtn.style.display === '',
     'диалог правки снова открыт, кнопка удаления видна');
  window.__fetches = [];
  eraseBtn.click();
  ok(window.__fetches.some(u => /\/api\/tracking\/erase/.test(u)),
     'клик «Удалить время» -> POST /api/tracking/erase: ' + decodeURIComponent(window.__fetches[0] || ''));
  await tick();
  ok(!modal.classList.contains('open'), 'после удаления модалка закрыта');
  ok(!cell(2).classList.contains('cal-done'), 'день снова не отмечен (✓ убран)');
  click(cell(2));
  ok(modal.classList.contains('open'), 'день снова кликабелен: диалог ввода открывается');
  APP.tracking.closeTrkDialog();
  ok(/Отметка удалена/.test(st.textContent), 'баннер: ' + st.textContent);
  ok(document.getElementById('trk-modal-info').style.display !== 'block',
     'подсказка скрыта (день больше не отмечен)');

  // --- формат месяца 10.2026 ---
  APP.calendar.showTable({ t: '10.2026', s1, s2, trk: null });  // trk:null -> loadTracking
  const cells2 = [...document.querySelectorAll('.cal-d[data-day]')];
  const we2 = cells2.filter(c => c.classList.contains('we')).map(c => +c.dataset.day);
  ok(JSON.stringify(we2) === JSON.stringify([3,4,10,11,17,18,24,25,31]),
     'формат 10.2026 распознан, выходные: ' + we2.join(','));
  ok(window.__fetches.some(u => /\/api\/tracking\/days\?month=/.test(u)),
     'loadTracking запросил отметки');

  // --- кнопки месяцев: build once + делегирование ---
  APP.app.showMonths();
  const mbtns = document.querySelectorAll('#months button.b');
  ok(mbtns.length === 2 && mbtns[1].classList.contains('active'), 'кнопки месяцев: 2, активна вторая');
  window.__fetches = [];
  click(mbtns[0]);
  ok(window.__fetches.some(u => /\/api\/month\?name=/.test(u) && decodeURIComponent(u).indexOf('Сентябрь') >= 0),
     'тап по месяцу -> запрос месяца: ' + decodeURIComponent(window.__fetches.join(',')));
  APP.app.showMonths();
  ok(document.querySelectorAll('#months button.b').length === 2 &&
     document.querySelector('#months button.b').classList.contains('active'),
     'кнопки не пересоздаются, .active переключается');

  // --- тема: переключатель, сохранение выбора, иконка ---
  const tbtn = document.getElementById('theme-toggle');
  ok(!!tbtn && !!document.querySelector('meta[name="theme-color"]'),
     'кнопка темы и meta theme-color размечены');
  window.localStorage.removeItem('theme');
  APP.app.initTheme();
  const root = document.documentElement;
  ok(root.getAttribute('data-theme') === 'light',
     'без сохранённого выбора и Telegram — светлая тема: ' + root.getAttribute('data-theme'));
  ok(tbtn.textContent === '🌙' && tbtn.getAttribute('aria-pressed') === 'false',
     'иконка светлой темы — луна (переключит на тёмную)');
  click(tbtn);
  ok(root.getAttribute('data-theme') === 'dark' && window.localStorage.getItem('theme') === 'dark',
     'тап -> тёмная тема, выбор сохранён в localStorage');
  ok(tbtn.textContent === '☀️' && tbtn.getAttribute('aria-pressed') === 'true',
     'иконка тёмной темы — солнце');
  ok(document.querySelector('meta[name="theme-color"]').getAttribute('content') === '#14161a',
     'meta theme-color обновлён под тёмную тему');
  APP.app.initTheme();   // повторная инициализация не должна сбрасывать выбор
  ok(root.getAttribute('data-theme') === 'dark',
     'сохранённый выбор переживает повторный init');
  click(tbtn);
  ok(root.getAttribute('data-theme') === 'light' && window.localStorage.getItem('theme') === 'light',
     'повторный тап -> светлая тема, выбор сохранён');
  window.localStorage.removeItem('theme');
}

main().then(() => {
  console.log(fails ? ('\nFAILURES: ' + fails) : '\nALL TESTS PASSED');
  process.exit(fails ? 1 : 0);
}, e => {
  console.error('ERROR', e);
  process.exit(1);
});
