import { html, mount, qs, qsa, clearFieldErrors, setFieldError } from '../core/dom.js';
import { api } from '../core/api.js';
import { icon } from '../core/icons.js';
import { categoryEmoji, categoryTone } from '../core/categories.js';
import {
  avatar,
  confirmDialog,
  emptyState,
  errorBlock,
  friendlyError,
  loadingBlock,
  openModal,
  setBusy,
  toast,
} from '../core/ui.js';
import {
  centsToInput,
  displayName,
  formatDateTime,
  formatMoney,
  localDayRangeUtc,
  parseMoneyToCents,
  parseNonNegativeMoneyToCents,
  transactionTypeLabel,
  uuid,
} from '../core/format.js';

export function renderLedger({ root, gid, group, members, categories, currentUser }) {
  let activeController = null;
  let requestId = 0;
  let loading = true;
  let loadingMore = false;
  let loadError = null;
  let items = [];
  let nextCursor = null;
  let filters = { category_id: '', payer_id: '', fromDate: '', toDate: '' };

  mount(root, loadingBlock('正在翻阅流水…'));

  function filterQuery() {
    const range = localDayRangeUtc(filters.fromDate, filters.toDate);
    return {
      category_id: filters.category_id,
      payer_id: filters.payer_id,
      from: range.from,
      to: range.to,
      limit: 20,
    };
  }

  async function load({ reset = true } = {}) {
    const id = ++requestId;
    activeController?.abort();
    activeController = new AbortController();
    if (reset) {
      loading = true;
      items = [];
      nextCursor = null;
      loadError = null;
      render();
    } else {
      loadingMore = true;
      render();
    }
    const query = filterQuery();
    if (!reset && nextCursor) {
      query.cursor = nextCursor;
    }
    try {
      const page = await api.transactions(gid, query, { signal: activeController.signal });
      if (id !== requestId) {
        return;
      }
      items = reset ? (page.items || []) : [...items, ...(page.items || [])];
      nextCursor = page.next_cursor || null;
      loading = false;
      loadingMore = false;
      render();
    } catch (error) {
      if (error.aborted || id !== requestId) {
        return;
      }
      loading = false;
      loadingMore = false;
      loadError = error;
      render();
    }
  }

  function categoryOptions(selected) {
    return html`${categories.map((category) => html`
      <option value="${category.id}" ${String(category.id) === String(selected) ? 'selected' : ''}>${category.name}</option>
    `)}`;
  }

  function memberOptions(selected) {
    return html`${members.map((member) => html`
      <option value="${member.user_id}" ${String(member.user_id) === String(selected) ? 'selected' : ''}>${displayName(member)}</option>
    `)}`;
  }

  function render() {
    if (loading) {
      mount(root, loadingBlock('正在翻阅流水…'));
      return;
    }
    if (loadError) {
      mount(root, errorBlock(loadError));
      qs('[data-action="retry"]', root)?.addEventListener('click', () => load());
      return;
    }
    const expenses = items.filter((item) => Number(item.type) === 0).reduce((sum, item) => sum + Number(item.amount_cents || 0), 0);
    const incomes = items.filter((item) => Number(item.type) === 1).reduce((sum, item) => sum + Number(item.amount_cents || 0), 0);
    const canEdit = (item) => Number(item.created_by) === Number(currentUser.id) || group.role === 'owner';

    mount(root, html`
      <section class="tab-panel">
        <div class="page-heading-row">
          <div>
            <p class="eyebrow">The ledger</p>
            <h2 class="page-title">每一笔，都有来处</h2>
            <p class="page-subtitle">支出和收入都按整数分记账。均分除不尽时，余数会按参与人顺序分配。</p>
          </div>
          <div class="page-actions">
            <button class="button button-primary" type="button" data-action="new-transaction">${icon('plus')}记一笔</button>
          </div>
        </div>

        <form class="card filter-card" data-filter-form>
          <div class="filter-row">
            <label class="field">
              <span>分类</span>
              <select class="select" name="category_id">
                <option value="">全部分类</option>
                ${categoryOptions(filters.category_id)}
              </select>
            </label>
            <label class="field">
              <span>付款人</span>
              <select class="select" name="payer_id">
                <option value="">全部成员</option>
                ${memberOptions(filters.payer_id)}
              </select>
            </label>
            <label class="field">
              <span>开始日期</span>
              <input class="input" type="date" name="fromDate" value="${filters.fromDate}">
            </label>
            <label class="field">
              <span>结束日期</span>
              <input class="input" type="date" name="toDate" value="${filters.toDate}">
            </label>
            <button class="button button-secondary" type="submit">${icon('filter')}筛选</button>
          </div>
        </form>

        <div class="ledger-layout">
          <section class="card card-pad">
            <div class="card-heading">
              <div>
                <h2 class="card-title">账本流水</h2>
                <p class="card-subtitle">已加载 ${items.length} 笔 · 支出 ${formatMoney(expenses)} · 收入 ${formatMoney(incomes)}</p>
              </div>
              ${Object.values(filters).some(Boolean) ? html`<button class="button button-link" type="button" data-action="clear-filter">清除筛选</button>` : ''}
            </div>
            ${items.length ? html`
              <div class="transaction-list">
                ${items.map((item) => {
                  const income = Number(item.type) === 1;
                  return html`
                    <article class="transaction-row">
                      <span class="transaction-icon ${income ? 'income' : ''}">${categoryEmoji({ name: item.category_name })}</span>
                      <div class="transaction-main">
                        <p class="transaction-title">${item.note || item.category_name || '未分类交易'}</p>
                        <p class="transaction-meta">
                          <span>${item.category_name || '未分类'}</span>
                          <span>${item.payer_nickname} 付款</span>
                          <span>${formatDateTime(item.created_at)}</span>
                          <span>${item.split_type === 0 ? '均分' : '自定义分摊'}</span>
                        </p>
                      </div>
                      <div class="transaction-people">
                        <span class="badge">${income ? '收入' : '支出'}</span>
                        ${item.split_type === 0 ? '均分' : '自定义'}
                      </div>
                      <div class="transaction-right">
                        <span class="transaction-amount ${income ? 'text-income' : 'text-expense'}">${income ? '+' : '-'}${formatMoney(item.amount_cents)}</span>
                        ${canEdit(item) ? html`
                          <div class="transaction-menu">
                            <button class="icon-button" type="button" data-edit-transaction="${item.id}" aria-label="编辑交易">${icon('edit', 'icon-sm')}</button>
                            <button class="icon-button danger" type="button" data-delete-transaction="${item.id}" aria-label="删除交易">${icon('trash', 'icon-sm')}</button>
                          </div>
                        ` : ''}
                      </div>
                    </article>
                  `;
                })}
              </div>
              <div class="load-more-row">
                ${nextCursor ? html`<button class="button button-secondary" type="button" data-action="load-more" ${loadingMore ? 'disabled' : ''}>${loadingMore ? '加载中…' : '加载更多'}</button>` : html`<span class="muted text-xs">已经到底了</span>`}
              </div>
            ` : emptyState(
              Object.values(filters).some(Boolean) ? '没有符合筛选条件的流水' : '还没有流水',
              Object.values(filters).some(Boolean) ? '换一个分类或清空日期范围再试试。' : '记下第一笔共同开销，余额和统计会自动更新。',
              html`<button class="button button-soft" type="button" data-action="new-transaction">${icon('plus')}记一笔</button>`,
              'ledger',
            )}
          </section>

          <aside class="ledger-side">
            <section class="card card-pad">
              <div class="card-heading"><div><h2 class="card-title">已加载摘要</h2><p class="card-subtitle">仅统计当前已加载的流水分页</p></div></div>
              <div class="mini-stat-grid">
                <div class="mini-stat"><span>支出</span><strong class="text-expense">${formatMoney(expenses)}</strong></div>
                <div class="mini-stat"><span>收入</span><strong class="text-income">${formatMoney(incomes)}</strong></div>
                <div class="mini-stat"><span>笔数</span><strong>${items.length}</strong></div>
                <div class="mini-stat"><span>净支出</span><strong>${formatMoney(expenses - incomes)}</strong></div>
              </div>
            </section>

            <section class="card card-pad">
              <div class="card-heading"><div><h2 class="card-title">可用分类</h2><p class="card-subtitle">系统分类和本组自定义分类</p></div></div>
              <div class="category-chips">
                ${categories.map((category, index) => html`
                  <span class="category-chip"><span class="category-icon ${categoryTone(index)}">${categoryEmoji(category)}</span>${category.name}</span>
                `)}
              </div>
            </section>

            <section class="card card-pad">
              <div class="notice">${icon('info')}<span>金额输入以元为单位，提交前会自动转成整数分。重复点击提交不会产生重复交易。</span></div>
            </section>
          </aside>
        </div>
      </section>
    `);
    bind();
  }

  function bind() {
    qs('[data-action="new-transaction"]', root)?.addEventListener('click', () => openTransactionModal(null));
    qsa('[data-action="new-transaction"]', root).forEach((button) => {
      if (!button.closest('.empty-state')) {
        return;
      }
      button.addEventListener('click', () => openTransactionModal(null));
    });
    qs('[data-filter-form]', root)?.addEventListener('submit', (event) => {
      event.preventDefault();
      const form = event.currentTarget;
      filters = {
        category_id: form.elements.category_id.value,
        payer_id: form.elements.payer_id.value,
        fromDate: form.elements.fromDate.value,
        toDate: form.elements.toDate.value,
      };
      load();
    });
    qs('[data-action="clear-filter"]', root)?.addEventListener('click', () => {
      filters = { category_id: '', payer_id: '', fromDate: '', toDate: '' };
      load();
    });
    qs('[data-action="load-more"]', root)?.addEventListener('click', () => load({ reset: false }));
    qsa('[data-edit-transaction]', root).forEach((button) => button.addEventListener('click', () => editTransaction(button.dataset.editTransaction, button)));
    qsa('[data-delete-transaction]', root).forEach((button) => button.addEventListener('click', () => deleteTransaction(button.dataset.deleteTransaction, button)));
  }

  async function editTransaction(id, button) {
    setBusy(button, true, '…');
    try {
      const detail = await api.transaction(gid, id);
      openTransactionModal(detail);
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
    } finally {
      setBusy(button, false);
    }
  }

  async function deleteTransaction(id, button) {
    const yes = await confirmDialog({
      title: '删除这笔交易？',
      message: '删除后，该交易会从统计和余额中排除。这个操作不能撤销。',
      confirmText: '删除交易',
    });
    if (!yes) {
      return;
    }
    setBusy(button, true, '…');
    try {
      await api.deleteTransaction(gid, id);
      toast('交易已删除', 'success');
      await load();
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
      setBusy(button, false);
    }
  }

  function openTransactionModal(existing) {
    const editing = Boolean(existing);
    const initialType = existing ? Number(existing.type) : 0;
    const initialSplit = existing ? Number(existing.split_type) : 0;
    const participantIds = new Set((existing?.participants || []).map((participant) => String(participant.user_id)));
    if (!editing) {
      members.forEach((member) => participantIds.add(String(member.user_id)));
    }
    const initialAmountCents = Number(existing?.amount_cents || 0);
    const shareDrafts = new Map((existing?.participants || []).map((participant) => [String(participant.user_id), centsToInput(participant.share_cents)]));
    const clientToken = uuid();

    const modal = openModal({
      title: editing ? '编辑交易' : '记一笔共同账',
      size: 'lg',
      body: html`
        <form class="form-stack" data-transaction-form novalidate>
          <div class="segmented" role="group" aria-label="交易类型">
            <button type="button" class="${initialType === 0 ? 'is-active' : ''}" data-type-choice="0">支出</button>
            <button type="button" class="${initialType === 1 ? 'is-active' : ''}" data-type-choice="1">收入</button>
          </div>
          <input type="hidden" name="type" value="${initialType}">
          <div class="form-grid">
            <label class="field span-2">
              <span>金额（元）</span>
              <div class="input-prefix"><span>¥</span><input class="input" name="amount" inputmode="decimal" autocomplete="off" placeholder="0.00" value="${editing ? centsToInput(existing.amount_cents) : ''}" required></div>
            </label>
            <label class="field">
              <span>分类</span>
              <select class="select" name="category_id" required>
                ${categories.map((category) => html`<option value="${category.id}" ${String(category.id) === String(existing?.category_id ?? categories[0]?.id ?? 0) ? 'selected' : ''}>${categoryEmoji(category)} ${category.name}</option>`)}
              </select>
            </label>
            <label class="field">
              <span>付款人</span>
              <select class="select" name="payer_id" required>
                ${members.map((member) => html`<option value="${member.user_id}" ${String(member.user_id) === String(existing?.payer_id ?? currentUser.id) ? 'selected' : ''}>${displayName(member)}</option>`)}
              </select>
            </label>
            <label class="field span-2">
              <span>备注（可选）</span>
              <input class="input" name="note" maxlength="500" value="${existing?.note || ''}" placeholder="例如：周末超市、十月房租">
            </label>
          </div>

          <div class="field">
            <span class="field-label">参与人</span>
            <div class="participant-grid" data-participant-grid>
              ${members.map((member) => html`
                <label class="participant-option">
                  <input type="checkbox" name="participant" value="${member.user_id}" ${participantIds.has(String(member.user_id)) ? 'checked' : ''}>
                  ${avatar(member, { size: 'xs' })}
                  <span class="participant-name">${displayName(member)}</span>
                </label>
              `)}
            </div>
          </div>

          <div class="field">
            <span class="field-label">分摊方式</span>
            <div class="segmented" role="group" aria-label="分摊方式">
              <button type="button" class="${initialSplit === 0 ? 'is-active' : ''}" data-split-choice="0">均分</button>
              <button type="button" class="${initialSplit === 1 ? 'is-active' : ''}" data-split-choice="1">自定义金额</button>
            </div>
            <input type="hidden" name="split_type" value="${initialSplit}">
          </div>
          <div class="split-editor" data-split-editor ${initialSplit === 0 ? 'hidden' : ''}></div>
          <div data-form-error></div>
          <div class="inline-actions modal-form-actions">
            <button class="button button-ghost" type="button" data-action="cancel-transaction">取消</button>
            <button class="button button-primary" type="submit">${editing ? '保存修改' : '确认记账'}</button>
          </div>
        </form>
      `,
      actions: [],
    });

    const form = modal.overlay.querySelector('[data-transaction-form]');
    const splitEditor = modal.overlay.querySelector('[data-split-editor]');
    const participantGrid = modal.overlay.querySelector('[data-participant-grid]');
    const submit = form.querySelector('button[type="submit"]');
    const errorArea = form.querySelector('[data-form-error]');

    form.querySelector('[data-action="cancel-transaction"]').addEventListener('click', () => modal.close());

    form.querySelectorAll('[data-type-choice]').forEach((button) => button.addEventListener('click', () => {
      form.elements.type.value = button.dataset.typeChoice;
      form.querySelectorAll('[data-type-choice]').forEach((item) => item.classList.toggle('is-active', item === button));
    }));

    form.querySelectorAll('[data-split-choice]').forEach((button) => button.addEventListener('click', () => {
      form.elements.split_type.value = button.dataset.splitChoice;
      form.querySelectorAll('[data-split-choice]').forEach((item) => item.classList.toggle('is-active', item === button));
      renderSplitEditor();
    }));

    participantGrid.addEventListener('change', renderSplitEditor);
    form.elements.amount.addEventListener('input', renderSplitEditor);
    renderSplitEditor();

    function selectedMembers() {
      return [...participantGrid.querySelectorAll('input[name="participant"]:checked')]
        .map((input) => members.find((member) => String(member.user_id) === input.value))
        .filter(Boolean);
    }

    function renderSplitEditor() {
      if (Number(form.elements.split_type.value) === 0 || splitEditor.hidden) {
        if (Number(form.elements.split_type.value) === 0) {
          splitEditor.hidden = true;
          splitEditor.replaceChildren();
        }
        return;
      }
      splitEditor.hidden = false;
      const selected = selectedMembers();
      const amount = parseMoneyToCents(form.elements.amount.value);
      mount(splitEditor, html`
        <p class="card-subtitle">为每位参与人填写承担金额，总计必须等于交易金额。</p>
        ${selected.map((member) => html`
          <div class="split-row">
            <span class="participant-name">${displayName(member)}</span>
            <div class="input-prefix"><span>¥</span><input class="input" inputmode="decimal" data-share-user="${member.user_id}" value="${shareDrafts.get(String(member.user_id)) || ''}" placeholder="0.00"></div>
          </div>
        `)}
        <div class="split-total">
          <span>分摊合计</span>
          <strong data-share-total>¥0.00</strong>
        </div>
      `);
      splitEditor.querySelectorAll('[data-share-user]').forEach((input) => {
        input.addEventListener('input', () => {
          shareDrafts.set(input.dataset.shareUser, input.value);
          updateTotal();
        });
      });
      updateTotal();

      function updateTotal() {
        const total = [...splitEditor.querySelectorAll('[data-share-user]')].reduce((sum, input) => {
          const parsed = parseNonNegativeMoneyToCents(input.value || '0');
          return sum + (parsed.ok ? parsed.cents : 0);
        }, 0);
        const totalElement = splitEditor.querySelector('[data-share-total]');
        if (totalElement) {
          totalElement.textContent = formatMoney(total);
          totalElement.classList.toggle('text-danger', amount.ok && total !== amount.cents);
          totalElement.classList.toggle('text-income', amount.ok && total === amount.cents);
        }
      }
    }

    form.addEventListener('submit', async (event) => {
      event.preventDefault();
      clearFieldErrors(form);
      mount(errorArea, '');
      const amount = parseMoneyToCents(form.elements.amount.value);
      if (!amount.ok) {
        setFieldError(form, 'amount', amount.error);
        form.elements.amount.focus();
        return;
      }
      const selected = selectedMembers();
      if (selected.length === 0) {
        mount(errorArea, html`<div class="notice notice-danger">${icon('alert')}<span>至少选择一位参与人。</span></div>`);
        return;
      }
      const splitType = Number(form.elements.split_type.value);
      let participants;
      if (splitType === 1) {
        let total = 0;
        participants = [];
        for (const member of selected) {
          const input = splitEditor.querySelector(`[data-share-user="${member.user_id}"]`);
          const parsed = parseNonNegativeMoneyToCents(input?.value || '');
          if (!parsed.ok) {
            mount(errorArea, html`<div class="notice notice-danger">${icon('alert')}<span>${displayName(member)} 的分摊金额格式不正确。</span></div>`);
            return;
          }
          const cents = parsed.cents;
          total += cents;
          participants.push({ user_id: Number(member.user_id), share_cents: cents });
        }
        if (total !== amount.cents) {
          mount(errorArea, html`<div class="notice notice-danger">${icon('alert')}<span>分摊合计必须等于交易金额 ${formatMoney(amount.cents)}，当前为 ${formatMoney(total)}。</span></div>`);
          return;
        }
      } else {
        participants = selected.map((member) => ({ user_id: Number(member.user_id) }));
      }
      const payload = {
        type: Number(form.elements.type.value),
        amount_cents: amount.cents,
        category_id: Number(form.elements.category_id.value),
        note: form.elements.note.value.trim(),
        payer_id: Number(form.elements.payer_id.value),
        split_type: splitType,
        participants,
      };
      if (!editing) {
        payload.client_token = clientToken;
      }
      setBusy(submit, true, editing ? '保存中…' : '记账中…');
      try {
        if (editing) {
          await api.updateTransaction(gid, existing.id, payload);
          toast('交易已更新', 'success');
        } else {
          await api.createTransaction(gid, payload);
          toast('已记下这笔共同开销', 'success');
        }
        modal.close();
        await load();
      } catch (error) {
        mount(errorArea, html`<div class="notice notice-danger">${icon('alert')}<span>${friendlyError(error)}</span></div>`);
        setBusy(submit, false);
      }
    });

    requestAnimationFrame(() => form.elements.amount.focus());
  }

  load();
  return { cleanup: () => activeController?.abort() };
}
