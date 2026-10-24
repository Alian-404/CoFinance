import { isValidDayValue, localDayValue } from './format.js';

const WEEKDAYS = ['一', '二', '三', '四', '五', '六', '日'];
let pickerSequence = 0;

function pad(value) {
  return String(value).padStart(2, '0');
}

function monthValue(year, month) {
  return `${String(year).padStart(4, '0')}-${pad(month)}`;
}

function boundMonth(value, fallback) {
  const text = String(value || '');
  return /^\d{4}-\d{2}/.test(text) ? text.slice(0, 7) : fallback;
}

export function monthFromValue(value, fallback = new Date()) {
  const text = String(value || '');
  if (/^\d{4}-\d{2}-\d{2}$/.test(text)) {
    return { year: Number(text.slice(0, 4)), month: Number(text.slice(5, 7)) };
  }
  return { year: fallback.getFullYear(), month: fallback.getMonth() + 1 };
}

export function clampMonth(year, month, { min = '', max = '' } = {}) {
  const today = monthValue(new Date().getFullYear(), new Date().getMonth() + 1);
  const lower = boundMonth(min, '0000-01');
  const upper = boundMonth(max, today);
  const lowerIndex = Number(lower.slice(0, 4)) * 12 + Number(lower.slice(5, 7)) - 1;
  const upperIndex = Number(upper.slice(0, 4)) * 12 + Number(upper.slice(5, 7)) - 1;
  const index = Math.min(upperIndex, Math.max(lowerIndex, year * 12 + month - 1));
  return { year: Math.floor(index / 12), month: (index % 12) + 1 };
}

export function shiftMonth(year, month, offset, bounds = {}) {
  const index = year * 12 + month - 1 + offset;
  return clampMonth(Math.floor(index / 12), (index % 12) + 1, bounds);
}

export function calendarCells(year, month, { min = '', max = '' } = {}) {
  const firstWeekday = (new Date(year, month - 1, 1).getDay() + 6) % 7;
  const daysInMonth = new Date(year, month, 0).getDate();
  const cells = Array(42).fill(null);
  for (let day = 1; day <= daysInMonth; day += 1) {
    const value = `${monthValue(year, month)}-${pad(day)}`;
    cells[firstWeekday + day - 1] = {
      day,
      value,
      disabled: !isValidDayValue(value, { min, max }),
    };
  }
  return cells;
}

export function monthLabel(year, month) {
  return `${year} 年 ${month} 月`;
}

export function createDatePicker({
  input,
  min = '2000-01-01',
  max = localDayValue(),
  onSelect,
}) {
  if (!input) {
    throw new Error('date picker requires an input');
  }
  const wrapper = input.closest('[data-date-picker]');
  if (!wrapper) {
    throw new Error('date picker requires a [data-date-picker] wrapper');
  }
  const trigger = wrapper.querySelector('[data-date-picker-trigger]');
  const popover = document.createElement('div');
  const popoverId = `date-picker-${++pickerSequence}`;
  popover.id = popoverId;
  popover.className = 'date-picker-popover';
  popover.hidden = true;
  popover.innerHTML = '<div class="date-picker-head"></div><div class="date-picker-weekdays"></div><div class="date-picker-grid" role="grid"></div><div class="date-picker-foot"></div>';
  wrapper.append(popover);
  let view = monthFromValue(input.value);
  let destroyed = false;
  let suppressFocusOpen = false;

  trigger?.setAttribute('aria-controls', popoverId);
  trigger?.setAttribute('aria-expanded', 'false');

  function choose(value) {
    input.value = value;
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.dispatchEvent(new Event('change', { bubbles: true }));
    onSelect?.(value);
    input.focus();
    close();
  }

  function render() {
    const label = monthLabel(view.year, view.month);
    popover.querySelector('.date-picker-head').innerHTML = `
      <button type="button" class="date-picker-nav" data-date-nav="-12" aria-label="上一年">«</button>
      <button type="button" class="date-picker-nav" data-date-nav="-1" aria-label="上个月">‹</button>
      <strong class="date-picker-title" aria-live="polite">${label}</strong>
      <button type="button" class="date-picker-nav" data-date-nav="1" aria-label="下个月">›</button>
      <button type="button" class="date-picker-nav" data-date-nav="12" aria-label="下一年">»</button>
    `;
    popover.querySelector('.date-picker-weekdays').innerHTML = WEEKDAYS
      .map((day) => `<span>${day}</span>`)
      .join('');
    popover.querySelector('.date-picker-grid').innerHTML = calendarCells(view.year, view.month, { min, max })
      .map((cell) => {
        if (!cell) {
          return '<span aria-hidden="true"></span>';
        }
        const classes = [
          'date-picker-day',
          cell.value === input.value ? 'is-selected' : '',
          cell.value === localDayValue() ? 'is-today' : '',
        ].filter(Boolean).join(' ');
        return `<button type="button" class="${classes}" data-date-day="${cell.value}" ${cell.disabled ? 'disabled' : ''} aria-label="${cell.value}">${cell.day}</button>`;
      })
      .join('');
    popover.querySelector('.date-picker-foot').innerHTML = `
      <button type="button" class="button button-link button-sm" data-date-today>今天</button>
      <button type="button" class="button button-link button-sm" data-date-clear>清除</button>
    `;

    popover.querySelectorAll('[data-date-nav]').forEach((button) => {
      button.addEventListener('click', () => {
        view = shiftMonth(view.year, view.month, Number(button.dataset.dateNav), { min, max });
        render();
      });
    });
    popover.querySelectorAll('[data-date-day]').forEach((button) => {
      button.addEventListener('click', () => choose(button.dataset.dateDay));
    });
    popover.querySelector('[data-date-today]').addEventListener('click', () => {
      if (isValidDayValue(localDayValue(), { min, max })) {
        choose(localDayValue());
      }
    });
    popover.querySelector('[data-date-clear]').addEventListener('click', () => choose(''));
  }

  function open() {
    if (destroyed || !popover.hidden) {
      return;
    }
    view = monthFromValue(input.value);
    view = clampMonth(view.year, view.month, { min, max });
    render();
    popover.hidden = false;
    trigger?.setAttribute('aria-expanded', 'true');
  }

  function close({ restoreFocus = false } = {}) {
    if (popover.hidden) {
      return;
    }
    popover.hidden = true;
    trigger?.setAttribute('aria-expanded', 'false');
    if (restoreFocus) {
      suppressFocusOpen = true;
      input.focus();
      suppressFocusOpen = false;
    }
  }

  function onInputFocus() {
    if (suppressFocusOpen) {
      return;
    }
    open();
  }

  function onInputKeydown(event) {
    if (event.key === 'ArrowDown') {
      event.preventDefault();
      open();
    } else if (event.key === 'Escape') {
      close();
    }
  }

  function onDocumentPointerDown(event) {
    if (!popover.hidden && !wrapper.contains(event.target)) {
      close();
    }
  }

  function onDocumentKeydown(event) {
    if (event.key === 'Escape' && !popover.hidden) {
      close({ restoreFocus: true });
    }
  }

  trigger?.addEventListener('click', () => {
    if (popover.hidden) {
      open();
    } else {
      close();
    }
  });
  input.addEventListener('focus', onInputFocus);
  input.addEventListener('click', onInputFocus);
  input.addEventListener('keydown', onInputKeydown);
  document.addEventListener('pointerdown', onDocumentPointerDown);
  document.addEventListener('keydown', onDocumentKeydown);

  return {
    open,
    close,
    destroy() {
      destroyed = true;
      input.removeEventListener('focus', onInputFocus);
      input.removeEventListener('click', onInputFocus);
      input.removeEventListener('keydown', onInputKeydown);
      document.removeEventListener('pointerdown', onDocumentPointerDown);
      document.removeEventListener('keydown', onDocumentKeydown);
      popover.remove();
    },
  };
}
