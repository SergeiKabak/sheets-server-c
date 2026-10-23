/**
 * Telegram Web App — collègues по кабинету (Вариант А)
 *
 * Подключите этот скрипт на страницу вашего Telegram Web App после основного скрипта,
 * где уже определены функции esc(), showTable(), переменные M, cur.
 *
 * Как подключить:
 *   <script src="telegram_webapp_cabinet.js"></script>
 *
 * Требования к серверу: эндпоинт GET /api/cabinet?month=...&cabinet=...&day=...
 * возвращает JSON: {cabinet, month, day, colleagues:[{name, shift}, ...]}
 */

/* 1. Патчим showTable: ячейки кабинетов становятся кликабельными */
(function () {
    var _origShowTable = window.showTable;
    if (typeof _origShowTable !== 'function') {
        console.warn('[cabinet.js] window.showTable не найдена. Подключите этот скрипт после основного.');
        return;
    }

    window.showTable = function (d) {
        // Вызываем оригинальную отрисовку
        _origShowTable(d);

        // После отрисовки таблицы вешаем обработчики на ячейки кабинета
        var w = document.getElementById('table');
        if (!w) return;

        var s1 = d.s1 || [];
        var s2 = d.s2 || [];
        var n = Math.max(s1.length, s2.length, 1);
        var days = n > 3 ? n - 3 : 0;

        // s1 — первая смена (Бинкевич Татьяна Эдуардовна), s2 — вторая
        [s1, s2].forEach(function (row, rowIndex) {
            for (var i = 0; i < days; i++) {
                var cab = row[i + 2] || '';
                if (!cab) continue;

                var cell = w.querySelector(
                    'tbody tr:nth-child(' + (rowIndex + 1) + ') td:nth-child(' + (i + 3) + ')'
                );
                if (!cell) continue;

                cell.dataset.day = i + 1;
                cell.dataset.cabinet = cab;
                cell.style.cursor = 'pointer';
                cell.style.textDecoration = 'underline dotted';
                cell.style.color = '#1a73e8';

                // Удаляем старый обработчик, если был, и вешаем новый
                cell.onclick = null;
                cell.addEventListener('click', function (e) {
                    e.stopPropagation();
                    var day = parseInt(this.dataset.day, 10);
                    var cabinet = this.dataset.cabinet;
                    if (cabinet) {
                        showColleagues(day, cabinet);
                    }
                });
            }
        });
    };
})();

/* 2. Функция получения коллег по кабинету и дню */
async function showColleagues(day, cabinet) {
    if (!cabinet) return;

    // Определяем текущий месяц (из глобальной переменной M и cur, если есть)
    var month = '';
    if (typeof M !== 'undefined' && Array.isArray(M) && typeof cur !== 'undefined' && M[cur]) {
        month = M[cur];
    }

    var url = '/api/cabinet?month=' + encodeURIComponent(month) +
              '&cabinet=' + encodeURIComponent(cabinet) +
              '&day=' + day;

    try {
        var res = await fetch(url);
        if (!res.ok) {
            throw new Error('HTTP ' + res.status);
        }
        var data = await res.json();
        var list = data.colleagues || [];

        var title = 'Кабинет ' + cabinet + ', день ' + day + (month ? ' (' + month + ')' : '');
        var text;

        if (list.length === 0) {
            text = title + '\n\nНикто другой не работает в этом кабинете в этот день.';
        } else {
            text = title + '\n\nВместе с Бинкевич Татьяна Эдуардовна:\n' +
                   list.map(function (c) {
                       return '• ' + c.name + (c.shift ? ' — ' + c.shift : '');
                   }).join('\n');
        }

        // Показываем алертом — можно заменить на модальное окно Telegram WebApp
        alert(text);
    } catch (e) {
        console.error('[cabinet.js]', e);
        alert('Ошибка загрузки коллег по кабинету ' + cabinet);
    }
}

/* 3. Небольшие стили для кликабельных ячеек (встраиваем в <head>, если ещё нет) */
(function () {
    if (document.getElementById('cabinet-js-styles')) return;
    var style = document.createElement('style');
    style.id = 'cabinet-js-styles';
    style.textContent = `
        td.cell[data-cabinet] {
            cursor: pointer;
            text-decoration: underline dotted;
            color: #1a73e8;
        }
        td.cell[data-cabinet]:hover {
            background-color: #e8f0fe;
        }
    `;
    document.head.appendChild(style);
})();