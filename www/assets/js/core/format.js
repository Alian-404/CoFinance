const currencyFormatter = new Intl.NumberFormat('zh-CN', {
  style: 'currency',
  currency: 'CNY',
  minimumFractionDigits: 2,
  maximumFractionDigits: 2,
});

const numberFormatter = new Intl.NumberFormat('zh-CN', {
  maximumFractionDigits: 2,
});

export function formatMoney(cents, { signed = false } = {}) {
  const value = Number(cents || 0) / 100;
  const formatted = currencyFormatter.format(Math.abs(value));
  if (!signed || value === 0) {
    return value < 0 ? `-${formatted}` : formatted;
  }
  return `${value > 0 ? '+' : '-'}${formatted}`;
}

export function formatNumber(value) {
  return numberFormatter.format(Number(value || 0));
}

export function parseMoneyToCents(input) {
  const parsed = parseNonNegativeMoneyToCents(input);
  if (!parsed.ok) {
    return parsed;
  }
  if (parsed.cents <= 0) {
    return { ok: false, error: '金额必须大于 0' };
  }
  return parsed;
}

export function parseNonNegativeMoneyToCents(input) {
  const text = String(input ?? '').trim().replace(/,/g, '');
  if (!/^\d+(?:\.\d{0,2})?$/.test(text)) {
    return { ok: false, error: '请输入最多两位小数的金额' };
  }
  const [whole, fraction = ''] = text.split('.');
  const cents = Number(whole) * 100 + Number(fraction.padEnd(2, '0'));
  if (!Number.isSafeInteger(cents) || cents < 0) {
    return { ok: false, error: '金额必须为非负数' };
  }
  if (cents > 999999999999) {
    return { ok: false, error: '金额超出可记录范围' };
  }
  return { ok: true, cents };
}

export function centsToInput(cents) {
  return (Number(cents || 0) / 100).toFixed(2);
}

export function parseDate(value) {
  if (!value) {
    return null;
  }
  const normalized = /Z$|[+-]\d{2}:\d{2}$/.test(value) ? value : `${value}Z`;
  const date = new Date(normalized);
  return Number.isNaN(date.getTime()) ? null : date;
}

export function formatDate(value) {
  const date = parseDate(value);
  if (!date) {
    return '—';
  }
  return new Intl.DateTimeFormat('zh-CN', {
    month: 'short',
    day: 'numeric',
  }).format(date);
}

export function formatDateTime(value) {
  const date = parseDate(value);
  if (!date) {
    return '—';
  }
  return new Intl.DateTimeFormat('zh-CN', {
    month: 'short',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  }).format(date);
}

export function formatTime(value) {
  const date = parseDate(value);
  if (!date) {
    return '—';
  }
  return new Intl.DateTimeFormat('zh-CN', {
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  }).format(date);
}

export function relativeTime(value) {
  const date = parseDate(value);
  if (!date) {
    return '';
  }
  const delta = date.getTime() - Date.now();
  const abs = Math.abs(delta);
  const formatter = new Intl.RelativeTimeFormat('zh-CN', { numeric: 'auto' });
  if (abs < 60_000) {
    return '刚刚';
  }
  if (abs < 3_600_000) {
    return formatter.format(Math.round(delta / 60_000), 'minute');
  }
  if (abs < 86_400_000) {
    return formatter.format(Math.round(delta / 3_600_000), 'hour');
  }
  if (abs < 604_800_000) {
    return formatter.format(Math.round(delta / 86_400_000), 'day');
  }
  return formatDate(value);
}

export function localDayValue(date = new Date()) {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  return `${year}-${month}-${day}`;
}

export function isValidDayValue(value, { min = '', max = '' } = {}) {
  const text = String(value || '').trim();
  if (!/^\d{4}-\d{2}-\d{2}$/.test(text)) {
    return false;
  }
  const [year, month, day] = text.split('-').map(Number);
  const date = new Date(year, month - 1, day);
  if (
    date.getFullYear() !== year ||
    date.getMonth() !== month - 1 ||
    date.getDate() !== day
  ) {
    return false;
  }
  return (!min || text >= min) && (!max || text <= max);
}

export function monthRangeUtc(offset = 0) {
  const now = new Date();
  const start = new Date(now.getFullYear(), now.getMonth() + offset, 1);
  const end = new Date(now.getFullYear(), now.getMonth() + offset + 1, 1);
  return { from: start.toISOString(), to: end.toISOString() };
}

export function localDayRangeUtc(fromValue, toValue) {
  const result = {};
  if (fromValue) {
    const date = new Date(`${fromValue}T00:00:00`);
    if (!Number.isNaN(date.getTime())) {
      result.from = date.toISOString();
    }
  }
  if (toValue) {
    const date = new Date(`${toValue}T00:00:00`);
    if (!Number.isNaN(date.getTime())) {
      date.setDate(date.getDate() + 1);
      result.to = date.toISOString();
    }
  }
  return result;
}

export function initials(name, fallback = '?') {
  const trimmed = String(name || '').trim();
  if (!trimmed) {
    return fallback;
  }
  const chars = [...trimmed];
  return chars.slice(0, 2).join('').toUpperCase();
}

export function displayName(person) {
  if (!person) {
    return '未知成员';
  }
  return person.nickname || person.username || person.sender_name || '未知成员';
}

export function roleLabel(role) {
  return role === 'owner' ? '创建者' : '成员';
}

export function transactionTypeLabel(type) {
  return Number(type) === 1 ? '收入' : '支出';
}

export function messagePreview(message) {
  if (!message) {
    return '还没有消息';
  }
  const content = String(message.content || '').replace(/\s+/g, ' ').trim();
  return content.length > 42 ? `${content.slice(0, 42)}…` : content;
}

export function uuid() {
  if (globalThis.crypto?.randomUUID) {
    return globalThis.crypto.randomUUID();
  }
  const bytes = new Uint8Array(16);
  globalThis.crypto?.getRandomValues?.(bytes);
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  const hex = [...bytes].map((value) => value.toString(16).padStart(2, '0')).join('');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

export function debounce(fn, delay = 250) {
  let timer = 0;
  return (...args) => {
    clearTimeout(timer);
    timer = window.setTimeout(() => fn(...args), delay);
  };
}
