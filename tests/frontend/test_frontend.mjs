import test from 'node:test';
import assert from 'node:assert/strict';

import {
  centsToInput,
  formatMoney,
  isValidDayValue,
  localDayRangeUtc,
  parseMoneyToCents,
  parseNonNegativeMoneyToCents,
  parseDate,
  uuid,
} from '../../www/assets/js/core/format.js';
import {
  groupMessagesByDay,
  latestMessageId,
  mergeMessages,
  nextReconnectDelay,
  overlapAfterId,
} from '../../www/assets/js/core/messages.js';
import { parseRoute } from '../../www/assets/js/core/router.js';
import { calendarCells, monthLabel, shiftMonth } from '../../www/assets/js/core/date-picker.js';
import { api, setUnauthorizedHandler } from '../../www/assets/js/core/api.js';
import { getToken, setToken } from '../../www/assets/js/core/session.js';

class MemoryStorage {
  constructor(entries = {}) {
    this.values = new Map(Object.entries(entries));
  }

  getItem(key) {
    return this.values.has(key) ? this.values.get(key) : null;
  }

  setItem(key, value) {
    this.values.set(key, String(value));
  }

  removeItem(key) {
    this.values.delete(key);
  }
}

test('金额解析拒绝零和负数，但允许非负分摊零值', () => {
  assert.deepEqual(parseMoneyToCents('12.34'), { ok: true, cents: 1234 });
  assert.deepEqual(parseMoneyToCents('1,234.5'), { ok: true, cents: 123450 });
  assert.equal(parseMoneyToCents('0.00').ok, false);
  assert.equal(parseMoneyToCents('-1').ok, false);
  assert.equal(parseMoneyToCents('1.234').ok, false);
  assert.equal(parseMoneyToCents('abc').ok, false);
  assert.deepEqual(parseNonNegativeMoneyToCents('0.00'), { ok: true, cents: 0 });
  assert.equal(parseNonNegativeMoneyToCents('0.001').ok, false);
  assert.equal(parseMoneyToCents('10000000000.00').ok, false);
  assert.equal(centsToInput(1), '0.01');
});

test('金额展示始终按整数分格式化', () => {
  assert.match(formatMoney(12345), /^¥123\.45$/);
  assert.match(formatMoney(-12345), /^-¥123\.45$/);
  assert.match(formatMoney(12345, { signed: true }), /^\+¥123\.45$/);
  assert.match(formatMoney(0, { signed: true }), /^¥0\.00$/);
});

test('服务端无时区时间按 UTC 解析', () => {
  const date = parseDate('2026-10-24T06:46:42');
  assert.equal(date.toISOString(), '2026-10-24T06:46:42.000Z');
  assert.equal(parseDate('not-a-date'), null);
});

test('消息合并按 id 排序并按 id 去重', () => {
  const existing = [
    { id: 3, content: 'three' },
    { id: 1, content: 'one' },
  ];
  const incoming = [
    { id: 3, content: 'three-new' },
    { id: 2, content: 'two' },
    { id: 4, content: 'four' },
  ];
  assert.deepEqual(mergeMessages(existing, incoming).map((item) => item.id), [1, 2, 3, 4]);
  assert.equal(mergeMessages(existing, incoming).find((item) => item.id === 3).content, 'three-new');
});

test('重连窗口和退避不超过上限', () => {
  assert.equal(overlapAfterId(100, 20), 80);
  assert.equal(overlapAfterId(5, 20), 0);
  assert.equal(nextReconnectDelay(0), 1000);
  assert.equal(nextReconnectDelay(3), 8000);
  assert.equal(nextReconnectDelay(10), 15000);
  assert.equal(latestMessageId([{ id: 2 }, { id: 9 }, { id: 1 }]), 9);
});

test('消息按自然日分组', () => {
  const groups = groupMessagesByDay([
    { id: 1, created_at: '2026-10-24T01:00:00Z' },
    { id: 2, created_at: '2026-10-24T02:00:00Z' },
    { id: 3, created_at: '2026-10-25T01:00:00Z' },
  ]);
  assert.deepEqual(groups.map((group) => group.messages.length), [2, 1]);
});

test('路由解析只接受合法的工作区标签', () => {
  assert.equal(parseRoute('#/login').name, 'auth');
  assert.equal(parseRoute('#/register').mode, 'register');
  assert.equal(parseRoute('#/groups').name, 'groups');
  assert.deepEqual(parseRoute('#/groups/42/ledger'), {
    name: 'workspace', gid: '42', tab: 'ledger', path: '/groups/42/ledger',
  });
  assert.equal(parseRoute('#/groups/42/unknown').tab, 'overview');
  assert.equal(parseRoute('#/groups/not-an-id').name, 'not-found');
  assert.equal(parseRoute('#/unknown').name, 'not-found');
});

test('客户端幂等键符合 UUID v4 形状', () => {
  assert.match(uuid(), /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i);
});

test('本地日期范围转成 UTC 半开区间', () => {
  const range = localDayRangeUtc('2026-10-24', '2026-10-25');
  assert.ok(range.from.endsWith('Z'));
  assert.ok(range.to.endsWith('Z'));
  assert.ok(new Date(range.from) < new Date(range.to));
});

test('日期筛选拒绝越界日期和不存在的自然日', () => {
  const limits = { min: '2000-01-01', max: '2026-10-24' };
  assert.equal(isValidDayValue('2026-10-24', limits), true);
  assert.equal(isValidDayValue('20666-01-01', limits), false);
  assert.equal(isValidDayValue('2026-02-30', limits), false);
  assert.equal(isValidDayValue('1999-12-31', limits), false);
  assert.equal(isValidDayValue('2026-10-25', limits), false);
});

test('日历按周一开头生成完整六周网格并禁用越界日期', () => {
  const cells = calendarCells(2026, 10, { min: '2000-01-01', max: '2026-10-24' });
  assert.equal(cells.length, 42);
  assert.equal(cells[3].value, '2026-10-01');
  assert.equal(cells[26].value, '2026-10-24');
  assert.equal(cells[26].disabled, false);
  assert.equal(cells[27].disabled, true);
  assert.equal(cells[3 + 30].value, '2026-10-31');
  assert.equal(monthLabel(2026, 10), '2026 年 10 月');
});

test('日历翻页支持跨年并限制在可选月份内', () => {
  const bounds = { min: '2000-01-01', max: '2026-10-24' };
  assert.deepEqual(shiftMonth(2026, 1, -1, bounds), { year: 2025, month: 12 });
  assert.deepEqual(shiftMonth(2026, 10, 1, bounds), { year: 2026, month: 10 });
  assert.deepEqual(shiftMonth(2026, 1, -1, { min: '2026-01-01', max: '2026-10-24' }), { year: 2026, month: 1 });
  assert.deepEqual(shiftMonth(2025, 12, 12, bounds), { year: 2026, month: 10 });
});

test('令牌写入标签页存储，并迁移旧的持久令牌', () => {
  const originalSession = globalThis.sessionStorage;
  const originalLocal = globalThis.localStorage;
  const tab = new MemoryStorage();
  const persistent = new MemoryStorage({ 'cofinance.token.v1': 'legacy-token' });
  globalThis.sessionStorage = tab;
  globalThis.localStorage = persistent;
  try {
    assert.equal(getToken(), 'legacy-token');
    assert.equal(tab.getItem('cofinance.token.v1'), 'legacy-token');
    assert.equal(persistent.getItem('cofinance.token.v1'), null);
    setToken('tab-a');
    assert.equal(tab.getItem('cofinance.token.v1'), 'tab-a');
    assert.equal(persistent.getItem('cofinance.token.v1'), null);
  } finally {
    setToken('');
    globalThis.sessionStorage = originalSession;
    globalThis.localStorage = originalLocal;
  }
});

test('旧密码错误不清理登录态，真实会话失效才清理', async () => {
  const originalSession = globalThis.sessionStorage;
  const originalLocal = globalThis.localStorage;
  const originalWindow = globalThis.window;
  const originalFetch = globalThis.fetch;
  let unauthorizedCalls = 0;
  globalThis.sessionStorage = new MemoryStorage();
  globalThis.localStorage = new MemoryStorage();
  globalThis.window = { setTimeout, clearTimeout };
  setUnauthorizedHandler(() => { unauthorizedCalls += 1; });
  setToken('session-a');
  try {
    globalThis.fetch = async () => new Response(JSON.stringify({ code: 1007, msg: '旧密码错误', data: null }), {
      status: 401,
      headers: { 'Content-Type': 'application/json' },
    });
    await assert.rejects(api.changePassword('wrong', 'NewPassword123'), (error) => error.code === 1007);
    assert.equal(getToken(), 'session-a');
    assert.equal(unauthorizedCalls, 0);

    globalThis.fetch = async () => new Response(JSON.stringify({ code: 1002, msg: '登录已失效', data: null }), {
      status: 401,
      headers: { 'Content-Type': 'application/json' },
    });
    await assert.rejects(api.me(), (error) => error.code === 1002);
    assert.equal(getToken(), '');
    assert.equal(unauthorizedCalls, 1);
  } finally {
    setUnauthorizedHandler(null);
    setToken('');
    globalThis.fetch = originalFetch;
    globalThis.window = originalWindow;
    globalThis.sessionStorage = originalSession;
    globalThis.localStorage = originalLocal;
  }
});
