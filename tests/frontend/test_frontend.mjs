import test from 'node:test';
import assert from 'node:assert/strict';

import {
  centsToInput,
  formatMoney,
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
