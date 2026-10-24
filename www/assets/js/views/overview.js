import { html, mount, qs, qsa } from '../core/dom.js';
import { api } from '../core/api.js';
import { icon } from '../core/icons.js';
import { categoryEmoji, categoryTone } from '../core/categories.js';
import { avatar, confirmDialog, emptyState, errorBlock, friendlyError, loadingBlock, setBusy, toast } from '../core/ui.js';
import {
  displayName,
  formatMoney,
  monthRangeUtc,
  relativeTime,
  transactionTypeLabel,
} from '../core/format.js';

const ranges = {
  this: { label: '本月', query: () => monthRangeUtc(0) },
  last: { label: '上月', query: () => monthRangeUtc(-1) },
  all: { label: '全部', query: () => ({}) },
};

export function renderOverview({ root, gid, group, members, goTab }) {
  const controller = new AbortController();
  let range = 'this';
  let loading = true;
  let loadError = null;
  let stats = null;
  let balances = null;
  let recent = [];
  let suggestions = null;
  let suggestionsLoading = false;
  let aiAnswer = '';
  let aiCached = false;
  let aiError = '';
  let aiLoading = false;

  mount(root, loadingBlock('正在汇总这本账…'));

  async function loadData() {
    loading = true;
    loadError = null;
    render();
    try {
      const query = ranges[range].query();
      const [statsResult, balanceResult, transactionResult] = await Promise.all([
        api.stats(gid, query, { signal: controller.signal }),
        api.balances(gid, { signal: controller.signal }),
        api.transactions(gid, { limit: 5 }, { signal: controller.signal }),
      ]);
      stats = statsResult;
      balances = balanceResult;
      recent = transactionResult.items || [];
      suggestions = null;
      loading = false;
      render();
    } catch (error) {
      if (error.aborted) {
        return;
      }
      loading = false;
      loadError = error;
      render();
    }
  }

  function render() {
    if (loading) {
      mount(root, loadingBlock('正在汇总这本账…'));
      return;
    }
    if (loadError) {
      mount(root, errorBlock(loadError));
      qs('[data-action="retry"]', root)?.addEventListener('click', loadData);
      return;
    }
    const summary = stats?.summary || {};
    const expenseCategories = stats?.expense_categories || [];
    const balanceItems = balances?.items || [];
    const activeBalances = balanceItems.filter((item) => Number(item.balance_cents) !== 0);
    const maxCategory = Math.max(1, ...expenseCategories.map((item) => Number(item.amount_cents || 0)));

    mount(root, html`
      <section class="tab-panel">
        <div class="page-heading-row">
          <div>
            <p class="eyebrow">At a glance</p>
            <h2 class="page-title">这本账，现在怎么样</h2>
            <p class="page-subtitle">${group.name} · ${members.length} 位成员 · 金额均以人民币分存储，展示时自动格式化。</p>
          </div>
          <div class="page-actions">
            <button class="button button-secondary" type="button" data-action="refresh">${icon('refresh')}刷新</button>
            <button class="button button-primary" type="button" data-action="go-ledger">${icon('plus')}记一笔</button>
          </div>
        </div>

        <div class="overview-grid">
          <div class="overview-main">
            <section class="overview-hero">
              <div class="overview-hero-head">
                <div>
                  <p class="hero-label">${ranges[range].label}共同支出</p>
                  <p class="hero-amount">${formatMoney(summary.total_expense_cents || 0)}</p>
                </div>
                <div class="range-picker" role="group" aria-label="统计时间范围">
                  ${Object.entries(ranges).map(([key, value]) => html`
                    <button type="button" class="${key === range ? 'is-active' : ''}" data-range="${key}">${value.label}</button>
                  `)}
                </div>
              </div>
              <div class="hero-foot">
                <div class="hero-metric"><span>收入</span><strong>${formatMoney(summary.total_income_cents || 0)}</strong></div>
                <div class="hero-metric"><span>当前金额</span><strong>${formatMoney((summary.total_income_cents || 0) - (summary.total_expense_cents || 0), { signed: true })}</strong></div>
                <div class="hero-metric"><span>交易</span><strong>${summary.transaction_count || 0} 笔</strong></div>
                <div class="hero-metric"><span>待结清人数</span><strong>${activeBalances.length} 人</strong></div>
              </div>
            </section>

            <section class="card card-pad">
              <div class="card-heading">
                <div>
                  <h2 class="card-title">钱花在了哪里</h2>
                  <p class="card-subtitle">${ranges[range].label}支出分类占比</p>
                </div>
                <button class="button button-link" type="button" data-action="go-ledger">查看流水</button>
              </div>
              ${expenseCategories.length ? html`
                <div class="category-bars">
                  ${expenseCategories.slice(0, 7).map((category, index) => {
                    const ratio = Math.max(0, Math.min(100, Number(category.ratio || 0)));
                    const relative = Number(category.amount_cents || 0) / maxCategory * 100;
                    return html`
                      <div class="category-bar">
                        <div class="category-bar-head">
                          <span class="category-name"><span class="category-icon">${categoryEmoji(category)}</span>${category.name}</span>
                          <span class="category-value">${formatMoney(category.amount_cents)} · ${ratio.toFixed(1)}%</span>
                        </div>
                        <progress class="bar-progress ${categoryTone(index)}" max="100" value="${relative.toFixed(2)}">${relative.toFixed(1)}%</progress>
                      </div>
                    `;
                  })}
                </div>
              ` : emptyState('还没有支出分类数据', '记下第一笔支出后，这里会显示分类占比。', html`<button class="button button-soft" type="button" data-action="go-ledger">去记一笔</button>`, 'overview')}
            </section>

            <section class="card card-pad">
              <div class="card-heading">
                <div>
                  <h2 class="card-title">最近的流水</h2>
                  <p class="card-subtitle">按创建时间倒序</p>
                </div>
                <button class="button button-link" type="button" data-action="go-ledger">全部流水</button>
              </div>
              ${recent.length ? html`
                <div class="transaction-list">
                  ${recent.map((item) => {
                    const income = Number(item.type) === 1;
                    return html`
                      <div class="transaction-row">
                        <span class="transaction-icon ${income ? 'income' : ''}">${categoryEmoji({ name: item.category_name })}</span>
                        <div class="transaction-main">
                          <p class="transaction-title">${item.note || item.category_name || '未分类交易'}</p>
                          <p class="transaction-meta"><span>${item.category_name || '未分类'}</span><span>${item.payer_nickname} 付款</span><span>${relativeTime(item.created_at)}</span></p>
                        </div>
                        <div class="transaction-people">${item.split_type === 0 ? '均分' : '自定义分摊'}</div>
                        <div class="transaction-right">
                          <span class="transaction-amount ${income ? 'text-income' : 'text-expense'}">${income ? '+' : '-'}${formatMoney(item.amount_cents)}</span>
                        </div>
                      </div>
                    `;
                  })}
                </div>
              ` : emptyState('还没有流水', '这本账的第一笔记录会出现在这里。', html`<button class="button button-soft" type="button" data-action="go-ledger">去记账</button>`, 'ledger')}
            </section>
          </div>

          <aside class="overview-side">
            <section class="card card-pad">
              <div class="card-heading">
                <div>
                  <h2 class="card-title">当前余额</h2>
                  <p class="card-subtitle">正数为应收，负数为应付</p>
                </div>
                <span class="badge ${activeBalances.length ? 'badge-amber' : 'badge-green'}">${activeBalances.length ? '待结清' : '已平'}</span>
              </div>
              ${balanceItems.length ? html`
                <div class="balance-list">
                  ${balanceItems.map((item) => html`
                    <div class="balance-row">
                      ${avatar(item, { size: 'sm' })}
                      <div class="person-copy">
                        <span class="person-name">${displayName(item)}</span>
                        <span class="person-meta">${Number(item.balance_cents) > 0 ? '应收' : Number(item.balance_cents) < 0 ? '应付' : '已结清'}</span>
                      </div>
                      <span class="balance-amount ${Number(item.balance_cents) > 0 ? 'text-income' : Number(item.balance_cents) < 0 ? 'text-expense' : 'muted'}">${formatMoney(item.balance_cents, { signed: true })}</span>
                    </div>
                  `)}
                </div>
              ` : emptyState('还没有成员余额', '成员加入并产生交易后会显示在这里。', null, 'users')}
            </section>

            <section class="card card-pad">
              <div class="card-heading">
                <div>
                  <h2 class="card-title">最少转账结算</h2>
                  <p class="card-subtitle">系统会复核当前余额后再确认</p>
                </div>
              </div>
              <div data-settlement-area>
                ${suggestions === null ? html`
                  <button class="button button-secondary button-wide" type="button" data-action="suggest" ${suggestionsLoading ? 'disabled' : ''}>${suggestionsLoading ? '正在计算…' : '生成结算建议'}</button>
                  ${activeBalances.length === 0 ? html`<p class="muted text-xs mt-8">当前余额已平，不需要转账。</p>` : ''}
                ` : suggestions.length === 0 ? html`
                  <div class="notice">${icon('check')}<span>当前没有需要结清的余额。</span></div>
                ` : html`
                  <div class="settlement-list">
                    ${suggestions.map((item, index) => html`
                      <div class="settlement-row">
                        <div class="settlement-flow">
                          <strong>${item.from_nickname}</strong>${icon('arrow')}<strong>${item.to_nickname}</strong>
                        </div>
                        <span class="settlement-amount">${formatMoney(item.amount_cents)}</span>
                        <button class="button button-sm button-soft" type="button" data-confirm-settlement="${index}">确认</button>
                      </div>
                    `)}
                  </div>
                  <button class="button button-link mt-8" type="button" data-action="suggest">重新计算</button>
                `}
              </div>
            </section>

            <section class="card card-pad ai-card">
              <div class="ai-heading">
                <span class="ai-orb">${icon('sparkles')}</span>
                <div>
                  <h2 class="card-title">AI 省钱建议</h2>
                  <p class="card-subtitle">只基于聚合统计生成</p>
                </div>
              </div>
              ${aiError ? html`<div class="notice notice-danger mt-12">${icon('alert')}<span>${aiError}</span></div>` : html`<div class="ai-copy">${aiAnswer}</div>`}
              <div class="inline-actions mt-12">
                <button class="button button-secondary button-sm" type="button" data-action="ai-suggest" ${aiLoading ? 'disabled' : ''}>${aiLoading ? '正在思考…' : aiAnswer ? '再生成一次' : '生成建议'}</button>
                ${aiCached ? html`<span class="badge badge-amber">缓存结果</span>` : ''}
              </div>
              <p class="ai-note">不会发送备注、聊天、用户名或逐笔明细。外部模型不可用时会自动降级。</p>
            </section>
          </aside>
        </div>
      </section>
    `);
    bind();
  }

  function bind() {
    qsa('[data-range]', root).forEach((button) => button.addEventListener('click', () => {
      if (button.dataset.range === range) {
        return;
      }
      range = button.dataset.range;
      loadData();
    }));
    qsa('[data-action="go-ledger"]', root).forEach((button) => button.addEventListener('click', () => goTab('ledger')));
    qs('[data-action="refresh"]', root)?.addEventListener('click', loadData);
    qs('[data-action="suggest"]', root)?.addEventListener('click', loadSuggestions);
    qsa('[data-confirm-settlement]', root).forEach((button) => button.addEventListener('click', () => confirmSettlement(Number(button.dataset.confirmSettlement), button)));
    qs('[data-action="ai-suggest"]', root)?.addEventListener('click', loadAiSuggestion);
  }

  async function loadSuggestions() {
    suggestionsLoading = true;
    render();
    try {
      const result = await api.suggestSettlements(gid);
      suggestions = result.items || [];
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
    } finally {
      suggestionsLoading = false;
      render();
    }
  }

  async function confirmSettlement(index, button) {
    const item = suggestions?.[index];
    if (!item) {
      return;
    }
    const yes = await confirmDialog({
      title: '确认这笔结算？',
      message: `确认 ${item.from_nickname} 已向 ${item.to_nickname} 转账 ${formatMoney(item.amount_cents)}。`,
      details: '结算记录会写入账本。请确保线下转账已经完成；确认后余额会立即更新。',
      confirmText: '确认已转账',
    });
    if (!yes) {
      return;
    }
    setBusy(button, true, '确认中…');
    try {
      await api.confirmSettlement(gid, item);
      toast('结算已记录', 'success');
      await loadData();
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
      setBusy(button, false);
    }
  }

  async function loadAiSuggestion() {
    aiLoading = true;
    aiError = '';
    render();
    try {
      const result = await api.aiSuggestions(gid);
      aiAnswer = result.answer || '';
      aiCached = Boolean(result.cached);
    } catch (error) {
      aiError = friendlyError(error);
    } finally {
      aiLoading = false;
      render();
    }
  }

  loadData();
  return { cleanup: () => controller.abort() };
}
