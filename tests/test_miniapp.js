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
    if (url.indexOf('/api/cabinet') >= 0) {
      // реалистичный ответ коллег
      return Promise.resolve({
        status: 200,
        text: () => Promise.resolve(JSON.stringify({
          colleagues: [{ name: 'Иванов Иван', shift: '7:30-14:40' },
                       { name: 'Петров Пётр', shift: '15:10-21:00' }]
        }))
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
  ok(!modal.classList.contains('open'), 'отмеченный день: модалка не открывается');
  ok(window.__alerts.length === alertsBefore + 1 && /Приход: 07:30/.test(window.__alerts[0]) &&
     /Отработано: 6.5 ч/.test(window.__alerts[0]) && /Сумма: 650.00/.test(window.__alerts[0]),
     'отработанное время показано: ' + JSON.stringify(window.__alerts[0]));

  click(cell(6));
  ok(!modal.classList.contains('open') && window.__alerts.length === alertsBefore + 1,
     'день без смены: тихо');

  // --- коллеги: модалка в стиле диалога времени (не alert) ---
  window.__fetches = [];
  const alertsSnap = window.__alerts.length;
  click(cell(1).querySelector('.cal-cab'));
  ok(window.__fetches.length === 1 && /\/api\/cabinet\?month=/.test(window.__fetches[0]),
     'тап по кабинету -> запрос коллег: ' + decodeURIComponent(window.__fetches[0]));
  ok(!modal.classList.contains('open'), 'модалка времени не открывается');
  ok(colModal.classList.contains('open'), 'открывается модалка коллег (как диалог времени)');
  ok(/Кабинет K2\.1/.test(document.getElementById('colleagues-title').textContent),
     'заголовок: ' + document.getElementById('colleagues-title').textContent);
  await tick();
  const listHtml = document.getElementById('colleagues-list').innerHTML;
  ok(/Иванов Иван/.test(listHtml) && /Петров Пётр/.test(listHtml) && /7:30-14:40/.test(listHtml),
     'список коллег отрисован с именами и сменами');
  ok(/class="col-row"/.test(listHtml), 'строки списка в стиле диалога (.col-row)');
  ok(window.__alerts.length === alertsSnap, 'alert() не используется для коллег');

  document.getElementById('colleagues-close').click();
  ok(!colModal.classList.contains('open'), 'кнопка «Закрыть» закрывает модалку коллег');
  click(cell(1).querySelector('.cal-cab'));
  await tick();
  colModal.dispatchEvent(new window.MouseEvent('click', { bubbles: true }));  // клик по подложке
  ok(!colModal.classList.contains('open'), 'клик по подложке закрывает модалку коллег');

  // --- защита прямого вызова диалога времени ---
  APP.tracking.openTrkDialog(6, '', '');
  ok(!modal.classList.contains('open') && /смен нет/.test(st.textContent),
     'openTrkDialog отклоняет день без смены: ' + st.textContent);
  APP.tracking.openTrkDialog(2, '', '');
  ok(!modal.classList.contains('open'), 'openTrkDialog отклоняет отмечённый день');

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
}

main().then(() => {
  console.log(fails ? ('\nFAILURES: ' + fails) : '\nALL TESTS PASSED');
  process.exit(fails ? 1 : 0);
}, e => {
  console.error('ERROR', e);
  process.exit(1);
});
