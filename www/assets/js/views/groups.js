import { html, mount, raw, qs, qsa } from '../core/dom.js';
import { api } from '../core/api.js';
import { state } from '../core/store.js';
import { navigate, groupPath } from '../core/router.js';
import { icon } from '../core/icons.js';
import { avatar, emptyState, errorBlock, friendlyError, loadingBlock, openModal, confirmDialog, setBusy, toast } from '../core/ui.js';
import { displayName, formatDateTime, initials, messagePreview, relativeTime, roleLabel } from '../core/format.js';
import { clearToken } from '../core/session.js';

function brand() {
  return html`
    <a class="brand topbar-mobile-brand" href="#/groups" aria-label="CoFinance 组列表">
      <span class="brand-mark" aria-hidden="true">C</span>
      <span class="brand-copy">
        <span class="brand-name">CoFinance</span>
        <span class="brand-tagline">Shared money, clearly</span>
      </span>
    </a>
  `;
}

function groupColor(index) {
  return ['', 'warm', 'blue'][index % 3];
}

export function renderGroups({ onSignedOut }) {
  const app = document.getElementById('app');
  let controller = new AbortController();
  let loading = true;
  let loadError = null;
  let invites = [];

  mount(app, html`
    <div class="main-shell">
      <header class="topbar">
        ${brand()}
        <div class="topbar-actions">
          <button class="icon-button notification-button" type="button" data-action="open-invites" aria-label="查看邀请">
            ${icon('invite')}
            <span class="notification-dot" data-invite-count hidden>0</span>
          </button>
          <button class="button button-secondary button-sm" type="button" data-action="create-group">${icon('plus')}<span>新建组</span></button>
          <button class="topbar-user" type="button" data-action="user-menu" aria-label="账号菜单">
            ${state.user ? avatar(state.user, { size: 'sm' }) : ''}
            <span class="topbar-user-name">${state.user ? displayName(state.user) : '账号'}</span>
            ${icon('down', 'icon-sm')}
          </button>
        </div>
      </header>

      <main class="page-content">
        <div class="page-heading-row">
          <div>
            <p class="eyebrow">Your shared spaces</p>
            <h1 class="page-title">今天，和谁一起记账？</h1>
            <p class="page-subtitle">每个组都是一本独立的共同账本。组内成员可以记账、聊天，并把余额结清。</p>
          </div>
          <div class="page-actions">
            <button class="button button-primary" type="button" data-action="create-group">${icon('plus')}创建共同账本</button>
          </div>
        </div>
        <div id="groups-content">${loadingBlock('正在取回你的组…')}</div>
        <div id="groups-aside"></div>
      </main>
    </div>
  `);

  const content = qs('#groups-content', app);
  const aside = qs('#groups-aside', app);

  function updateInviteBadge() {
    const badge = qs('[data-invite-count]', app);
    if (!badge) {
      return;
    }
    badge.hidden = invites.length === 0;
    badge.textContent = invites.length > 9 ? '9+' : String(invites.length);
  }

  function renderAside() {
    const groupCount = state.groups.length;
    const unread = state.groups.reduce((sum, group) => sum + Number(group.unread_count || 0), 0);
    mount(aside, html`
      <div class="groups-aside">
        <section class="aside-illustration">
          <h3>账清楚，关系才更轻松。</h3>
          <p>记一笔共同消费，系统会自动分摊到每个人。月底不用翻聊天记录，也能知道谁该转给谁。</p>
          <div class="aside-stat">
            <div><strong>${groupCount}</strong><span>共同账本</span></div>
            <div><strong>${unread}</strong><span>未读消息</span></div>
          </div>
        </section>
        <section class="card card-pad">
          <div class="card-heading">
            <div>
              <h2 class="card-title">待处理邀请</h2>
              <p class="card-subtitle">接受后会出现在左侧与组列表中</p>
            </div>
            <span class="badge ${invites.length ? 'badge-amber' : ''}">${invites.length}</span>
          </div>
          <div class="invite-list" id="inline-invites"></div>
        </section>
      </div>
    `);
    paintInvites(qs('#inline-invites', aside), true);
  }

  function paintInvites(target, inline = false) {
    if (!target) {
      return;
    }
    if (invites.length === 0) {
      mount(target, html`<p class="muted text-xs">暂时没有待处理邀请。</p>`);
      return;
    }
    mount(target, html`${invites.map((invite) => html`
      <div class="invite-row">
        <span class="group-avatar warm">${initials(invite.group_name, '组')}</span>
        <div class="invite-copy">
          <strong>${invite.group_name}</strong>
          <span>${invite.inviter_username} 邀请 · ${relativeTime(invite.created_at)}</span>
        </div>
        <div class="invite-actions">
          <button class="button button-sm button-primary" type="button" data-accept-invite="${invite.id}">接受</button>
          ${inline ? html`<button class="icon-button danger" type="button" data-reject-invite="${invite.id}" aria-label="拒绝邀请">${icon('close')}</button>` : html`<button class="button button-sm button-ghost" type="button" data-reject-invite="${invite.id}">拒绝</button>`}
        </div>
      </div>
    `)}`);
    target.querySelectorAll('[data-accept-invite]').forEach((button) => {
      button.addEventListener('click', () => handleInvite(button.dataset.acceptInvite, true, button));
    });
    target.querySelectorAll('[data-reject-invite]').forEach((button) => {
      button.addEventListener('click', () => handleInvite(button.dataset.rejectInvite, false, button));
    });
  }

  function renderGroupsContent() {
    if (loading) {
      mount(content, html`<div class="groups-layout"><div>${loadingBlock('正在取回你的组…')}</div><div>${loadingBlock('正在检查邀请…')}</div></div>`);
      return;
    }
    if (loadError) {
      mount(content, html`
        <div class="groups-layout">
          <div>${errorBlock(loadError)}</div>
          <div></div>
        </div>
      `);
      qs('[data-action="retry"]', content)?.addEventListener('click', load);
      return;
    }
    const cards = state.groups.length
      ? state.groups.map((group, index) => html`
          <a class="group-card" href="#${groupPath(group.id, 'overview')}">
            <div class="group-card-head">
              <span class="group-avatar ${groupColor(index)}">${initials(group.name, '组')}</span>
              <span class="badge ${group.role === 'owner' ? 'badge-green' : ''}">${roleLabel(group.role)}</span>
            </div>
            <h2 class="group-card-title">${group.name}</h2>
            <div class="group-card-meta">
              <span>${icon('users', 'icon-sm')}</span>
              <span>${group.member_count} 位成员</span>
              ${Number(group.unread_count) > 0 ? html`<span class="count-badge">${group.unread_count > 99 ? '99+' : group.unread_count}</span>` : ''}
            </div>
            <div class="group-card-last">
              <span class="truncate">${group.last_message ? `${group.last_message.sender_name || '成员'}：${messagePreview(group.last_message)}` : '还没有聊天消息，先记第一笔账吧'}</span>
              <span class="group-card-arrow">${icon('arrow', 'icon-sm')}</span>
            </div>
          </a>
        `)
      : emptyState('还没有共同账本', '创建第一个组，把室友、伴侣或家人邀请进来。', html`<button class="button button-primary" type="button" data-action="create-group">${icon('plus')}创建第一个组</button>`, 'users');
    mount(content, html`
      <div class="groups-layout">
        <section>
          <div class="groups-grid">${cards}</div>
        </section>
        <div id="aside-slot"></div>
      </div>
    `);
    const slot = qs('#aside-slot', content);
    slot.replaceWith(aside);
    renderAside();
    qsa('[data-action="create-group"]', content).forEach((button) => button.addEventListener('click', openCreateGroup));
  }

  async function load() {
    controller.abort();
    controller = new AbortController();
    loading = true;
    loadError = null;
    renderGroupsContent();
    try {
      const [groupsResult, inviteResult] = await Promise.all([
        api.groups({ signal: controller.signal }),
        api.invites({ signal: controller.signal }),
      ]);
      state.groups = groupsResult.items || [];
      state.groupsLoaded = true;
      invites = inviteResult.items || [];
      state.invites = invites;
      loading = false;
      updateInviteBadge();
      renderGroupsContent();
    } catch (error) {
      if (error.aborted) {
        return;
      }
      loading = false;
      loadError = error;
      renderGroupsContent();
    }
  }

  async function handleInvite(inviteId, accept, button) {
    setBusy(button, true, accept ? '接受中…' : '拒绝中…');
    try {
      let groupId = null;
      if (accept) {
        const result = await api.acceptInvite(inviteId);
        groupId = result?.group_id;
        toast('已加入共同账本', 'success');
      } else {
        await api.rejectInvite(inviteId);
        toast('已拒绝邀请', 'info');
      }
      invites = invites.filter((invite) => String(invite.id) !== String(inviteId));
      state.invites = invites;
      updateInviteBadge();
      await load();
      if (accept && groupId) {
        navigate(groupPath(groupId, 'overview'));
      }
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
    } finally {
      setBusy(button, false);
    }
  }

  function openInvites() {
    const modal = openModal({
      title: '待处理邀请',
      body: html`<div class="invite-list" data-modal-invites></div>`,
      size: 'md',
      actions: [{ label: '关闭', className: 'button-secondary', onClick: () => modal.close() }],
    });
    paintInvites(modal.overlay.querySelector('[data-modal-invites]'), false);
  }

  function openCreateGroup() {
    const modal = openModal({
      title: '创建共同账本',
      size: 'sm',
      body: html`
        <form class="dialog-form" data-create-group-form>
          <label class="field">
            <span>账本名称</span>
            <input class="input" name="name" placeholder="例如：南门 302、我们的家" maxlength="50" required autocomplete="off">
            <small class="field-hint">创建后你就是 owner，可以邀请成员并把账本整理成你们习惯的样子。</small>
          </label>
          <button class="button button-primary button-wide" type="submit">创建并进入账本</button>
        </form>
      `,
      actions: [],
    });
    const form = modal.overlay.querySelector('[data-create-group-form]');
    form.addEventListener('submit', async (event) => {
      event.preventDefault();
      const name = form.elements.name.value.trim();
      if (!name) {
        toast('请填写账本名称', 'warning');
        return;
      }
      const submit = qs('button[type="submit"]', form);
      setBusy(submit, true, '正在创建…');
      try {
        const group = await api.createGroup(name);
        modal.close();
        toast('共同账本已创建', 'success');
        navigate(groupPath(group.id, 'overview'));
      } catch (error) {
        toast(friendlyError(error), 'error', 4800);
        setBusy(submit, false);
      }
    });
    requestAnimationFrame(() => form.elements.name.focus());
  }

  function openUserMenu() {
    if (!state.user) {
      return;
    }
    const modal = openModal({
      title: '账号',
      size: 'sm',
      body: html`
        <div class="account-summary">
          ${avatar(state.user, { size: 'lg' })}
          <div class="account-summary-copy">
            <h3>${displayName(state.user)}</h3>
            <p class="muted">@${state.user.username}</p>
          </div>
        </div>
        <div class="notice">${icon('info')}<span>昵称、头像、密码与会话管理都在任意账本的「设置 → 账号」中。加入一个账本后即可完整管理账号。</span></div>
      `,
      actions: [
        { label: '取消', className: 'button-ghost', onClick: () => modal.close() },
        { label: '退出登录', className: 'button-danger', onClick: () => signOut(modal) },
      ],
    });
  }

  async function signOut(modal) {
    const yes = await confirmDialog({
      title: '退出当前账号？',
      message: '退出后当前浏览器需要重新登录。其他设备上的会话不受影响。',
      confirmText: '退出登录',
    });
    if (!yes) {
      return;
    }
    try {
      await api.logout();
    } catch {
      // Clear the local session even if the server token was already expired.
    }
    clearToken();
    state.user = null;
    state.groups = [];
    state.groupsLoaded = false;
    modal.close();
    onSignedOut?.();
    navigate('/login');
  }

  app.querySelectorAll('[data-action="create-group"]').forEach((button) => button.addEventListener('click', openCreateGroup));
  qs('[data-action="open-invites"]', app)?.addEventListener('click', openInvites);
  qs('[data-action="user-menu"]', app)?.addEventListener('click', openUserMenu);
  updateInviteBadge();
  load();

  return {
    cleanup() {
      controller.abort();
    },
  };
}
