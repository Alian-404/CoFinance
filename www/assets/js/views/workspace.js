import { html, mount, qs, qsa } from '../core/dom.js';
import { api } from '../core/api.js';
import { state, resetProtectedState } from '../core/store.js';
import { navigate, groupPath } from '../core/router.js';
import { icon } from '../core/icons.js';
import {
  avatar,
  closeModal,
  confirmDialog,
  emptyState,
  errorBlock,
  friendlyError,
  loadingBlock,
  openModal,
  setBusy,
  toast,
} from '../core/ui.js';
import { displayName, initials, relativeTime, roleLabel } from '../core/format.js';
import { clearToken } from '../core/session.js';
import { renderOverview } from './overview.js';
import { renderLedger } from './ledger.js';
import { renderChat } from './chat.js';
import { renderSettings } from './settings.js';

const tabs = [
  { key: 'overview', label: '概览', icon: 'overview' },
  { key: 'ledger', label: '账本', icon: 'ledger' },
  { key: 'chat', label: '聊天', icon: 'chat' },
  { key: 'settings', label: '设置', icon: 'settings' },
];

function groupColor(index) {
  return ['', 'warm', 'blue'][index % 3];
}

export function renderWorkspace({ gid, tab, onSignedOut }) {
  const app = document.getElementById('app');
  const initialTab = tabs.some((item) => item.key === tab) ? tab : 'overview';
  let dataController = new AbortController();
  let currentTab = initialTab;
  let currentCleanup = null;
  let group = null;
  let members = [];
  let categories = [];
  let invites = [];
  let loading = true;
  let loadError = null;

  mount(app, loadingBlock('正在打开共同账本…'));

  async function loadData() {
    dataController.abort();
    dataController = new AbortController();
    loading = true;
    loadError = null;
    mount(app, html`<div class="workspace-shell">${loadingBlock('正在打开共同账本…')}</div>`);
    try {
      const [groupResult, memberResult, categoryResult, groupsResult, inviteResult] = await Promise.all([
        api.group(gid, { signal: dataController.signal }),
        api.members(gid, { signal: dataController.signal }),
        api.categories(gid, { signal: dataController.signal }),
        api.groups({ signal: dataController.signal }),
        api.invites({ signal: dataController.signal }),
      ]);
      group = groupResult;
      members = memberResult.items || [];
      categories = categoryResult.items || [];
      invites = inviteResult.items || [];
      state.currentGroup = group;
      state.members = members;
      state.categories = categories;
      state.groups = groupsResult.items || [];
      state.groupsLoaded = true;
      state.invites = invites;
      loading = false;
      renderShell();
      mountTab(currentTab);
    } catch (error) {
      if (error.aborted) {
        return;
      }
      loading = false;
      loadError = error;
      if (error.status === 403 || error.status === 404 || error.code === 1004) {
        toast(friendlyError(error), 'error', 4800);
        navigate('/groups', { replace: true });
        return;
      }
      renderFailure();
    }
  }

  function renderFailure() {
    mount(app, html`
      <div class="not-found">
        <span class="empty-icon">${icon('alert')}</span>
        <h1>账本没有打开</h1>
        <p>${friendlyError(loadError)}</p>
        <div class="inline-actions">
          <button class="button button-secondary" type="button" data-action="retry">重试</button>
          <a class="button button-primary" href="#/groups">返回组列表</a>
        </div>
      </div>
    `);
    qs('[data-action="retry"]', app)?.addEventListener('click', loadData);
  }

  function sidebarTemplate() {
    return html`
      <aside class="sidebar" data-sidebar>
        <div class="sidebar-head">
          <a class="brand" href="#/groups" aria-label="返回所有账本">
            <span class="brand-mark" aria-hidden="true">C</span>
            <span class="brand-copy"><span class="brand-name">CoFinance</span><span class="brand-tagline">Shared money, clearly</span></span>
          </a>
          <button class="icon-button" type="button" data-action="close-sidebar" aria-label="关闭导航">${icon('close', 'icon-sm')}</button>
        </div>
        <div class="sidebar-section">
          <div class="sidebar-label"><span>共同账本</span><a href="#/groups" aria-label="所有账本">${icon('right', 'icon-sm')}</a></div>
          <nav class="group-nav" aria-label="共同账本">
            ${state.groups.map((item, index) => html`
              <a class="group-nav-item ${String(item.id) === String(gid) ? 'is-active' : ''}" href="#${groupPath(item.id, currentTab)}" data-group-nav="${item.id}">
                <span class="group-avatar ${groupColor(index)}">${initials(item.name, '组')}</span>
                <span class="group-nav-copy"><span class="group-nav-name">${item.name}</span><span class="group-nav-meta">${item.member_count} 人 · ${item.role === 'owner' ? 'owner' : '成员'}</span></span>
                ${Number(item.unread_count) > 0 ? html`<span class="count-badge">${item.unread_count > 99 ? '99+' : item.unread_count}</span>` : ''}
              </a>
            `)}
          </nav>
        </div>
        <div class="sidebar-footer">
          <a class="button button-secondary button-wide" href="#/groups">${icon('plus')}管理或新建组</a>
          <div class="sidebar-account">
            ${avatar(state.user, { size: 'sm' })}
            <div class="sidebar-account-copy"><span class="sidebar-account-name">${displayName(state.user)}</span><span class="sidebar-account-handle">@${state.user?.username || ''}</span></div>
            <button class="icon-button" type="button" data-action="user-settings" aria-label="账号设置">${icon('settings', 'icon-sm')}</button>
          </div>
        </div>
      </aside>
      <div class="sidebar-scrim" data-sidebar-scrim hidden></div>
    `;
  }

  function renderShell() {
    mount(app, html`
      <div class="app-frame workspace-shell">
        ${sidebarTemplate()}
        <div class="main-shell">
          <header class="workspace-topbar">
            <div class="workspace-title-group">
              <button class="icon-button" type="button" data-action="open-sidebar" aria-label="打开导航">${icon('menu')}</button>
              <span class="group-avatar">${initials(group.name, '组')}</span>
              <div>
                <h1 class="workspace-title" data-workspace-name>${group.name}</h1>
                <p class="workspace-meta" data-workspace-meta>${members.length} 位成员 · ${roleLabel(group.role)}</p>
              </div>
            </div>
            <div class="topbar-actions">
              <button class="icon-button notification-button" type="button" data-action="workspace-invites" aria-label="查看邀请">
                ${icon('invite')}
                <span class="notification-dot" data-invite-count ${invites.length ? '' : 'hidden'}>${invites.length > 9 ? '9+' : invites.length}</span>
              </button>
              <button class="topbar-user" type="button" data-action="user-settings" aria-label="打开账号设置">
                ${avatar(state.user, { size: 'sm' })}
                <span class="topbar-user-name">${displayName(state.user)}</span>
              </button>
            </div>
          </header>
          <nav class="workspace-tabs" aria-label="账本页面">
            ${tabs.map((item) => html`
              <a class="workspace-tab ${item.key === currentTab ? 'is-active' : ''}" href="#${groupPath(gid, item.key)}" data-tab="${item.key}">${icon(item.icon)}<span>${item.label}</span></a>
            `)}
          </nav>
          <main class="workspace-main" data-workspace-content>${loadingBlock('正在加载页面…')}</main>
        </div>
      </div>
    `);
    bindShell();
  }

  function bindShell() {
    qs('[data-action="open-sidebar"]', app)?.addEventListener('click', openSidebar);
    qs('[data-action="close-sidebar"]', app)?.addEventListener('click', closeSidebar);
    qs('[data-sidebar-scrim]', app)?.addEventListener('click', closeSidebar);
    qs('[data-action="workspace-invites"]', app)?.addEventListener('click', openInvites);
    qsa('[data-action="user-settings"]', app).forEach((button) => button.addEventListener('click', () => {
      closeSidebar();
      navigate(groupPath(gid, 'settings'));
    }));
    qsa('[data-group-nav]', app).forEach((link) => link.addEventListener('click', closeSidebar));
  }

  function openSidebar() {
    qs('[data-sidebar]', app)?.classList.add('is-open');
    qs('[data-sidebar-scrim]', app).hidden = false;
  }

  function closeSidebar() {
    qs('[data-sidebar]', app)?.classList.remove('is-open');
    const scrim = qs('[data-sidebar-scrim]', app);
    if (scrim) {
      scrim.hidden = true;
    }
  }

  function updateShellMeta() {
    const name = qs('[data-workspace-name]', app);
    const meta = qs('[data-workspace-meta]', app);
    if (name) {
      name.textContent = group.name;
    }
    if (meta) {
      meta.textContent = `${members.length} 位成员 · ${roleLabel(group.role)}`;
    }
    const sidebarLink = qs(`[data-group-nav="${gid}"] .group-nav-name`, app);
    if (sidebarLink) {
      sidebarLink.textContent = group.name;
    }
    const sidebarMeta = qs(`[data-group-nav="${gid}"] .group-nav-meta`, app);
    if (sidebarMeta) {
      sidebarMeta.textContent = `${members.length} 人 · ${group.role === 'owner' ? 'owner' : '成员'}`;
    }
  }

  function updateInviteBadge() {
    const badge = qs('[data-invite-count]', app);
    if (badge) {
      badge.hidden = invites.length === 0;
      badge.textContent = invites.length > 9 ? '9+' : String(invites.length);
    }
  }

  function mountTab(tab) {
    currentCleanup?.();
    currentCleanup = null;
    currentTab = tab;
    qsa('[data-tab]', app).forEach((link) => link.classList.toggle('is-active', link.dataset.tab === tab));
    const content = qs('[data-workspace-content]', app);
    if (!content) {
      return;
    }
    mount(content, '');
    const common = {
      root: content,
      gid,
      group,
      members,
      categories,
      currentUser: state.user,
      invites,
      goTab: (nextTab) => navigate(groupPath(gid, nextTab)),
    };
    if (tab === 'overview') {
      currentCleanup = renderOverview(common).cleanup;
    } else if (tab === 'ledger') {
      currentCleanup = renderLedger(common).cleanup;
    } else if (tab === 'chat') {
      currentCleanup = renderChat(common).cleanup;
    } else if (tab === 'settings') {
      currentCleanup = renderSettings({
        ...common,
        onGroupUpdated(updated) {
          group = { ...group, ...updated };
          state.currentGroup = group;
          const inList = state.groups.find((item) => String(item.id) === String(gid));
          if (inList) {
            inList.name = group.name;
            inList.role = group.role;
          }
          updateShellMeta();
        },
        onMembersChanged(nextMembers) {
          members = nextMembers;
          state.members = members;
          group.member_count = members.length;
          updateShellMeta();
        },
        onInvitesChanged(nextInvites) {
          invites = nextInvites;
          state.invites = invites;
          updateInviteBadge();
        },
        onInviteAccepted(nextGroupId) {
          state.groupsLoaded = false;
          navigate(groupPath(nextGroupId, 'overview'));
        },
        onUserUpdated(user) {
          state.user = user;
          renderShell();
          mountTab(currentTab);
        },
        onGroupDeleted() {
          state.groupsLoaded = false;
          navigate('/groups');
        },
        onSignedOut() {
          resetProtectedState();
          clearToken();
          onSignedOut?.();
          navigate('/login');
        },
      });
    }
  }

  function openInvites() {
    const modal = openModal({
      title: '待处理邀请',
      body: html`<div data-workspace-invites></div>`,
      actions: [{ label: '关闭', className: 'button-secondary', onClick: () => modal.close() }],
    });
    const target = modal.overlay.querySelector('[data-workspace-invites]');
    paintInvites(target, modal);
  }

  function paintInvites(target, modal) {
    if (!invites.length) {
      mount(target, emptyState('没有待处理邀请', '别人邀请你加入组后，会出现在这里。', null, 'invite'));
      return;
    }
    mount(target, html`<div class="invite-list">${invites.map((invite) => html`
      <div class="invite-row">
        <span class="group-avatar warm">${initials(invite.group_name, '组')}</span>
        <div class="invite-copy"><strong>${invite.group_name}</strong><span>${invite.inviter_username} 邀请 · ${relativeTime(invite.created_at)}</span></div>
        <div class="invite-actions">
          <button class="button button-sm button-primary" type="button" data-accept="${invite.id}">接受</button>
          <button class="button button-sm button-ghost" type="button" data-reject="${invite.id}">拒绝</button>
        </div>
      </div>
    `)}</div>`);
    target.querySelectorAll('[data-accept]').forEach((button) => button.addEventListener('click', () => handleInvite(button.dataset.accept, true, button, modal)));
    target.querySelectorAll('[data-reject]').forEach((button) => button.addEventListener('click', () => handleInvite(button.dataset.reject, false, button, modal)));
  }

  async function handleInvite(inviteId, accept, button, modal) {
    setBusy(button, true, accept ? '接受中…' : '拒绝中…');
    try {
      let newGroupId = null;
      if (accept) {
        const result = await api.acceptInvite(inviteId);
        newGroupId = result?.group_id;
        toast('已加入共同账本', 'success');
      } else {
        await api.rejectInvite(inviteId);
        toast('已拒绝邀请', 'info');
      }
      invites = invites.filter((item) => String(item.id) !== String(inviteId));
      state.invites = invites;
      updateInviteBadge();
      modal?.close();
      if (accept && newGroupId) {
        navigate(groupPath(newGroupId, 'overview'));
      } else if (currentTab === 'settings') {
        mountTab('settings');
      }
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
      setBusy(button, false);
    }
  }

  loadData();

  return {
    setTab(tab) {
      if (loading || loadError || !group) {
        currentTab = tab;
        return;
      }
      if (tab === currentTab && qs('[data-workspace-content]', app)?.hasChildNodes()) {
        return;
      }
      mountTab(tab);
    },
    cleanup() {
      dataController.abort();
      currentCleanup?.();
      currentCleanup = null;
      closeModal();
    },
  };
}
