import { html, mount, qs, qsa, clearFieldErrors, setFieldError } from '../core/dom.js';
import { api } from '../core/api.js';
import { icon } from '../core/icons.js';
import { categoryEmoji } from '../core/categories.js';
import {
  avatar,
  confirmDialog,
  friendlyError,
  openModal,
  setBusy,
  toast,
} from '../core/ui.js';
import { formatDate, displayName, initials, relativeTime, roleLabel } from '../core/format.js';
import { clearToken } from '../core/session.js';

const avatarChoices = [
  '',
  '/assets/avatars/leaf.svg',
  '/assets/avatars/sun.svg',
  '/assets/avatars/wave.svg',
  '/assets/avatars/pebble.svg',
  '/assets/avatars/moon.svg',
  '/assets/avatars/bloom.svg',
];

const iconChoices = [
  { value: 'restaurant', label: '餐饮 🍜' },
  { value: 'commute', label: '交通 🚇' },
  { value: 'shopping', label: '购物 🛍' },
  { value: 'home', label: '居住 🏠' },
  { value: 'entertainment', label: '娱乐 🎬' },
  { value: 'medical', label: '医疗 🏥' },
  { value: 'education', label: '教育 📚' },
  { value: 'income', label: '收入 💼' },
  { value: 'more', label: '其他 ✦' },
];

export function renderSettings({
  root,
  gid,
  group,
  members,
  categories,
  invites = [],
  currentUser,
  onGroupUpdated,
  onGroupDeleted,
  onUserUpdated,
  onMembersChanged,
  onInvitesChanged,
  onInviteAccepted,
  onSignedOut,
}) {
  let localGroup = { ...group };
  let localMembers = members.map((member) => ({ ...member }));
  let localCategories = categories.map((category) => ({ ...category }));
  let localInvites = invites.map((invite) => ({ ...invite }));
  let localUser = { ...currentUser };
  let selectedAvatar = localUser.avatar_url || '';
  let activeSection = 'group';
  let scrollFrame = 0;

  render();

  function render() {
    window.removeEventListener('scroll', scheduleSectionSync);
    if (scrollFrame) {
      window.cancelAnimationFrame(scrollFrame);
      scrollFrame = 0;
    }
    const isOwner = localGroup.role === 'owner';
    const customCategories = localCategories.filter((category) => Number(category.group_id) !== 0);
    const systemCategories = localCategories.filter((category) => Number(category.group_id) === 0);
    mount(root, html`
      <section class="tab-panel">
        <div class="page-heading-row">
          <div>
            <p class="eyebrow">Make it yours</p>
            <h2 class="page-title">设置与成员</h2>
            <p class="page-subtitle">管理这本账的成员、分类与生命周期，也管理你自己的账号安全。</p>
          </div>
        </div>

        <div class="settings-layout">
          <nav class="settings-nav" aria-label="设置分区">
            <button class="${activeSection === 'group' ? 'is-active' : ''}" type="button" data-settings-target="group">${icon('users')}组资料</button>
            <button class="${activeSection === 'members' ? 'is-active' : ''}" type="button" data-settings-target="members">${icon('invite')}成员与邀请</button>
            <button class="${activeSection === 'categories' ? 'is-active' : ''}" type="button" data-settings-target="categories">${icon('ledger')}分类管理</button>
            <button class="${activeSection === 'account' ? 'is-active' : ''}" type="button" data-settings-target="account">${icon('user')}账号与安全</button>
            <button class="${activeSection === 'danger' ? 'is-active' : ''}" type="button" data-settings-target="danger">${icon('alert')}危险操作</button>
          </nav>

          <div class="settings-content">
            <section class="card card-pad settings-section" id="settings-group">
              <div class="settings-section-head">
                <div><h3>组资料</h3><p>名称会展示给所有成员。只有 owner 可以修改。</p></div>
                <span class="badge ${isOwner ? 'badge-green' : ''}">${roleLabel(localGroup.role)}</span>
              </div>
              <form class="settings-form settings-form-narrow" data-group-name-form>
                <label class="field">
                  <span>账本名称</span>
                  <input class="input" name="name" value="${localGroup.name}" maxlength="50" ${isOwner ? '' : 'disabled'} required>
                </label>
                <div class="inline-actions">
                  <button class="button button-primary" type="submit" ${isOwner ? '' : 'disabled'}>保存名称</button>
                  <span class="muted text-xs">创建于 ${formatDate(localGroup.created_at)}</span>
                </div>
              </form>
            </section>

            <section class="card card-pad settings-section" id="settings-members">
              <div class="settings-section-head">
                <div><h3>成员与邀请</h3><p>单组最多 20 人，待接受邀请也占用名额，7 天未处理自动过期。</p></div>
                <button class="button button-primary button-sm" type="button" data-action="invite-member">${icon('invite')}邀请成员</button>
              </div>
              <div class="member-list">
                ${localMembers.map((member) => {
                  const self = Number(member.user_id) === Number(localUser.id);
                  return html`
                    <div class="member-row">
                      ${avatar(member, { size: 'md' })}
                      <div class="person-copy">
                        <span class="person-name">${displayName(member)} ${self ? html`<span class="badge badge-blue">我</span>` : ''}</span>
                        <span class="person-meta">@${member.username} · ${roleLabel(member.role)}${member.joined_at ? ` · ${formatDate(member.joined_at)} 加入` : ''}</span>
                      </div>
                      <div class="member-actions">
                        ${isOwner && !self && member.role !== 'owner' ? html`
                          <button class="button button-sm button-ghost" type="button" data-transfer-owner="${member.user_id}">转让 owner</button>
                          <button class="button button-sm button-ghost" type="button" data-remove-member="${member.user_id}">移除</button>
                        ` : ''}
                      </div>
                    </div>
                  `;
                })}
              </div>
              ${localInvites.length ? html`
                <div class="settings-invites">
                  <div class="settings-invites-head">
                    <div><strong>我收到的待处理邀请</strong><span>可以在当前页面直接接受或拒绝。</span></div>
                    <span class="badge badge-amber">${localInvites.length}</span>
                  </div>
                  <div class="settings-invite-list">
                    ${localInvites.map((invite) => html`
                      <div class="settings-invite-row">
                        <span class="group-avatar warm">${initials(invite.group_name, '组')}</span>
                        <div class="invite-copy">
                          <strong>${invite.group_name}</strong>
                          <span>@${invite.inviter_username} 邀请 · ${relativeTime(invite.created_at)}</span>
                        </div>
                        <div class="inline-actions">
                          <button class="button button-sm button-primary" type="button" data-accept-invite="${invite.id}">接受</button>
                          <button class="button button-sm button-ghost" type="button" data-reject-invite="${invite.id}">拒绝</button>
                        </div>
                      </div>
                    `)}
                  </div>
                </div>
              ` : ''}
            </section>

            <section class="card card-pad settings-section" id="settings-categories">
              <div class="settings-section-head">
                <div><h3>分类管理</h3><p>系统分类不可删除；组内自定义分类在有未删除交易时会被拒绝删除。</p></div>
              </div>
              <form class="form-grid" data-category-form>
                <label class="field">
                  <span>新分类名称</span>
                  <input class="input" name="name" maxlength="50" placeholder="例如：宠物、旅行" required>
                </label>
                <label class="field">
                  <span>图标</span>
                  <select class="select" name="icon">
                    ${iconChoices.map((choice) => html`<option value="${choice.value}">${choice.label}</option>`)}
                  </select>
                </label>
                <div class="span-2"><button class="button button-secondary" type="submit">${icon('plus')}添加分类</button></div>
              </form>
              <div class="divider"></div>
              <div class="category-manage-list">
                ${[...systemCategories, ...customCategories].map((category) => html`
                  <div class="category-manage-row">
                    <span class="category-icon">${categoryEmoji(category)}</span>
                    <div class="category-manage-copy">
                      <strong>${category.name}</strong>
                      <span>${Number(category.group_id) === 0 ? '系统预置' : '本组自定义'}</span>
                    </div>
                    ${Number(category.group_id) === 0 ? html`<span class="badge">只读</span>` : html`<button class="icon-button danger" type="button" data-delete-category="${category.id}" aria-label="删除分类">${icon('trash', 'icon-sm')}</button>`}
                  </div>
                `)}
              </div>
            </section>

            <section class="card card-pad settings-section" id="settings-account">
              <div class="settings-section-head">
                <div><h3>账号与安全</h3><p>这些设置会作用于你的所有账本。</p></div>
              </div>
              <div class="account-summary">
                ${avatar({ ...localUser, avatar_url: selectedAvatar }, { size: 'lg' })}
                <div class="account-summary-copy">
                  <h3>${displayName(localUser)}</h3>
                  <p class="muted">@${localUser.username} · 用户 ID ${localUser.id}</p>
                </div>
              </div>
              <form class="settings-form settings-form-narrow" data-profile-form>
                <label class="field">
                  <span>昵称</span>
                  <input class="input" name="nickname" value="${localUser.nickname || ''}" maxlength="50">
                </label>
                <input type="hidden" name="avatar_url" value="${selectedAvatar}">
                <div class="field">
                  <span class="field-label">预置头像</span>
                  <div class="avatar-picker">
                    ${avatarChoices.map((url) => html`
                      <button class="avatar-choice ${url === selectedAvatar ? 'is-selected' : ''}" type="button" data-avatar-choice="${url}" aria-label="${url ? '选择这个头像' : '使用昵称首字'}">
                        ${url ? avatar({ nickname: localUser.nickname, avatar_url: url }, { size: 'md' }) : html`<span class="avatar avatar-md avatar-fallback">${initials(displayName(localUser))}</span>`}
                      </button>
                    `)}
                  </div>
                </div>
                <button class="button button-primary" type="submit">保存个人资料</button>
              </form>
              <div class="divider"></div>
              <form class="settings-form settings-form-narrow" data-password-form>
                <h4 class="settings-subtitle">修改密码</h4>
                <label class="field"><span>当前密码</span><input class="input" type="password" name="old_password" autocomplete="current-password" required></label>
                <label class="field"><span>新密码</span><input class="input" type="password" name="new_password" autocomplete="new-password" minlength="8" maxlength="128" required></label>
                <label class="field"><span>确认新密码</span><input class="input" type="password" name="confirm" autocomplete="new-password" required></label>
                <button class="button button-secondary" type="submit">更新密码</button>
              </form>
              <div class="divider"></div>
              <div class="danger-row">
                <div><strong>会话管理</strong><p>退出当前设备，或让所有设备上的登录凭证失效。</p></div>
                <div class="inline-actions">
                  <button class="button button-sm button-secondary" type="button" data-action="logout">退出当前设备</button>
                  <button class="button button-sm button-danger" type="button" data-action="logout-all">退出所有设备</button>
                </div>
              </div>
            </section>

            <section class="card card-pad settings-section danger-zone" id="settings-danger">
              <div class="settings-section-head">
                <div><h3>危险操作</h3><p>离开、解散和注销都会影响账本或账号，请先完成必要的结算。</p></div>
              </div>
              <div class="danger-row">
                <div><strong>${isOwner ? '解散这个组' : '退出这个组'}</strong><p>${isOwner ? 'owner 可以正常解散；若仍有未结清余额，系统会要求二次确认强制解散。' : '退出前余额必须为 0，否则会返回明确提示。'}</p></div>
                ${isOwner ? html`<button class="button button-sm button-danger" type="button" data-action="dissolve">解散组</button>` : html`<button class="button button-sm button-danger" type="button" data-action="leave">退出组</button>`}
              </div>
              <div class="danger-row">
                <div><strong>注销账号</strong><p>账号会被软删除，历史交易保留但显示为已注销用户。仍是 owner 时必须先转让或解散组。</p></div>
                <button class="button button-sm button-danger" type="button" data-action="delete-account">注销账号</button>
              </div>
            </section>
          </div>
        </div>
      </section>
    `);
    bind();
  }

  function bind() {
    qsa('[data-settings-target]', root).forEach((button) => button.addEventListener('click', () => {
      setActiveSection(button.dataset.settingsTarget);
      qs(`#settings-${button.dataset.settingsTarget}`, root)?.scrollIntoView({ behavior: 'smooth', block: 'start' });
    }));
    window.addEventListener('scroll', scheduleSectionSync, { passive: true });
    scheduleSectionSync();
    qs('[data-group-name-form]', root)?.addEventListener('submit', saveGroupName);
    qs('[data-action="invite-member"]', root)?.addEventListener('click', openInviteModal);
    qsa('[data-accept-invite]', root).forEach((button) => button.addEventListener('click', () => respondToInvite(button.dataset.acceptInvite, true, button)));
    qsa('[data-reject-invite]', root).forEach((button) => button.addEventListener('click', () => respondToInvite(button.dataset.rejectInvite, false, button)));
    qsa('[data-transfer-owner]', root).forEach((button) => button.addEventListener('click', () => transferOwner(button.dataset.transferOwner, button)));
    qsa('[data-remove-member]', root).forEach((button) => button.addEventListener('click', () => removeMember(button.dataset.removeMember, button)));
    qs('[data-category-form]', root)?.addEventListener('submit', createCategory);
    qsa('[data-delete-category]', root).forEach((button) => button.addEventListener('click', () => deleteCategory(button.dataset.deleteCategory, button)));
    qs('[data-profile-form]', root)?.addEventListener('submit', saveProfile);
    qsa('[data-avatar-choice]', root).forEach((button) => button.addEventListener('click', () => {
      selectedAvatar = button.dataset.avatarChoice || '';
      qsa('[data-avatar-choice]', root).forEach((item) => item.classList.toggle('is-selected', item === button));
      qs('[data-profile-form] input[name="avatar_url"]', root).value = selectedAvatar;
    }));
    qs('[data-password-form]', root)?.addEventListener('submit', changePassword);
    qs('[data-action="logout"]', root)?.addEventListener('click', () => logout(false));
    qs('[data-action="logout-all"]', root)?.addEventListener('click', () => logout(true));
    qs('[data-action="leave"]', root)?.addEventListener('click', leaveGroup);
    qs('[data-action="dissolve"]', root)?.addEventListener('click', dissolveGroup);
    qs('[data-action="delete-account"]', root)?.addEventListener('click', deleteAccount);
  }

  function setActiveSection(section) {
    activeSection = section;
    qsa('[data-settings-target]', root).forEach((button) => {
      button.classList.toggle('is-active', button.dataset.settingsTarget === section);
    });
  }

  function scheduleSectionSync() {
    if (scrollFrame) {
      return;
    }
    scrollFrame = window.requestAnimationFrame(() => {
      scrollFrame = 0;
      const sections = qsa('.settings-section', root);
      let current = sections[0];
      for (const section of sections) {
        if (section.getBoundingClientRect().top <= 190) {
          current = section;
        }
      }
      if (current?.id.startsWith('settings-')) {
        setActiveSection(current.id.slice('settings-'.length));
      }
    });
  }

  async function saveGroupName(event) {
    event.preventDefault();
    const form = event.currentTarget;
    const submit = qs('button[type="submit"]', form);
    const name = form.elements.name.value.trim();
    if (!name) {
      toast('账本名称不能为空', 'warning');
      form.elements.name.focus();
      return;
    }
    setBusy(submit, true, '保存中…');
    try {
      const updated = await api.renameGroup(gid, name);
      localGroup = { ...localGroup, ...updated, role: localGroup.role, member_count: localGroup.member_count };
      onGroupUpdated?.(localGroup);
      toast('账本名称已更新', 'success');
      render();
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
      setBusy(submit, false);
    }
  }

  function openInviteModal() {
    const modal = openModal({
      title: '邀请成员',
      size: 'md',
      body: html`
        <div class="form-stack">
          <form class="inline-actions" data-invite-search>
            <input class="input" name="query" placeholder="按用户名搜索" autocomplete="off">
            <button class="button button-secondary" type="submit">${icon('search')}搜索</button>
          </form>
          <div data-search-results></div>
          <div class="divider"></div>
          <form class="form-stack" data-invite-form>
            <label class="field"><span>直接输入用户名邀请</span><input class="input" name="username" placeholder="对方注册时使用的用户名" autocomplete="off" required></label>
            <button class="button button-primary" type="submit">${icon('invite')}发送邀请</button>
          </form>
          <div class="notice">${icon('info')}<span>邀请 7 天内有效。待接受邀请也占用 20 人容量。</span></div>
        </div>
      `,
      actions: [],
    });
    const searchForm = modal.overlay.querySelector('[data-invite-search]');
    const results = modal.overlay.querySelector('[data-search-results]');
    const inviteForm = modal.overlay.querySelector('[data-invite-form]');
    searchForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const q = searchForm.elements.query.value.trim();
      if (!q) {
        return;
      }
      const button = qs('button[type="submit"]', searchForm);
      setBusy(button, true, '搜索中…');
      try {
        const data = await api.searchUsers(q);
        const items = data.items || [];
        mount(results, items.length ? html`${items.map((user) => html`
          <div class="invite-row">
            ${avatar(user, { size: 'sm' })}
            <div class="invite-copy"><strong>${displayName(user)}</strong><span>@${user.username}</span></div>
            <button class="button button-sm button-soft" type="button" data-invite-username="${user.username}">邀请</button>
          </div>
        `)}` : html`<p class="muted text-xs">没有找到匹配用户。</p>`);
        results.querySelectorAll('[data-invite-username]').forEach((item) => item.addEventListener('click', () => {
          inviteForm.elements.username.value = item.dataset.inviteUsername;
          inviteForm.requestSubmit();
        }));
      } catch (error) {
        toast(friendlyError(error), 'error', 4800);
      } finally {
        setBusy(button, false);
      }
    });
    inviteForm.elements.username.addEventListener('input', () => {
      results.replaceChildren();
    });
    inviteForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const button = qs('button[type="submit"]', inviteForm);
      const username = inviteForm.elements.username.value.trim();
      if (!username) {
        toast('请输入用户名', 'warning');
        return;
      }
      setBusy(button, true, '发送中…');
      try {
        await api.inviteMember(gid, username);
        toast(`已向 @${username} 发送邀请`, 'success');
        inviteForm.elements.username.value = '';
        results.replaceChildren();
      } catch (error) {
        toast(friendlyError(error), 'error', 4800);
      } finally {
        setBusy(button, false);
      }
    });
  }

  async function respondToInvite(inviteId, accept, button) {
    const invite = localInvites.find((item) => String(item.id) === String(inviteId));
    if (!invite) {
      return;
    }
    setBusy(button, true, accept ? '接受中…' : '拒绝中…');
    try {
      let joinedGroupId = null;
      if (accept) {
        const result = await api.acceptInvite(inviteId);
        joinedGroupId = result?.group_id;
        toast(`已加入「${invite.group_name}」`, 'success');
      } else {
        await api.rejectInvite(inviteId);
        toast(`已拒绝「${invite.group_name}」的邀请`, 'info');
      }
      localInvites = localInvites.filter((item) => String(item.id) !== String(inviteId));
      onInvitesChanged?.(localInvites);
      if (accept && joinedGroupId) {
        onInviteAccepted?.(joinedGroupId);
        return;
      }
      render();
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
      setBusy(button, false);
    }
  }

  async function transferOwner(userId, button) {
    const member = localMembers.find((item) => String(item.user_id) === String(userId));
    if (!member) {
      return;
    }
    const yes = await confirmDialog({
      title: '转让 owner？',
      message: `转让后，${displayName(member)} 将成为账本 owner，你会降为普通成员。`,
      details: '只有 owner 才能改名、邀请、移除成员和解散账本。该操作会立即生效。',
      confirmText: '确认转让',
    });
    if (!yes) {
      return;
    }
    setBusy(button, true, '转让中…');
    try {
      await api.transferOwner(gid, userId);
      localMembers = localMembers.map((item) => ({
        ...item,
        role: String(item.user_id) === String(userId) ? 'owner' : String(item.user_id) === String(localUser.id) ? 'member' : item.role,
      }));
      localGroup.role = 'member';
      onGroupUpdated?.(localGroup);
      onMembersChanged?.(localMembers);
      toast('owner 已转让', 'success');
      render();
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
      setBusy(button, false);
    }
  }

  async function removeMember(userId, button) {
    const member = localMembers.find((item) => String(item.user_id) === String(userId));
    if (!member) {
      return;
    }
    const yes = await confirmDialog({
      title: '移除成员？',
      message: `将 ${displayName(member)} 从这本账中移除。`,
      details: '如果对方仍有未结清余额，服务端会拒绝这次操作。',
      confirmText: '移除成员',
    });
    if (!yes) {
      return;
    }
    setBusy(button, true, '移除中…');
    try {
      await api.removeMember(gid, userId);
      localMembers = localMembers.filter((item) => String(item.user_id) !== String(userId));
      localGroup.member_count = localMembers.length;
      onMembersChanged?.(localMembers);
      onGroupUpdated?.(localGroup);
      toast('成员已移除', 'success');
      render();
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
      setBusy(button, false);
    }
  }

  async function createCategory(event) {
    event.preventDefault();
    const form = event.currentTarget;
    const button = qs('button[type="submit"]', form);
    const name = form.elements.name.value.trim();
    if (!name) {
      return;
    }
    setBusy(button, true, '添加中…');
    try {
      const category = await api.createCategory(gid, { name, icon: form.elements.icon.value, sort: 100 });
      localCategories = [...localCategories, category];
      toast('分类已添加', 'success');
      render();
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
      setBusy(button, false);
    }
  }

  async function deleteCategory(categoryId, button) {
    const category = localCategories.find((item) => String(item.id) === String(categoryId));
    if (!category) {
      return;
    }
    const yes = await confirmDialog({
      title: '删除分类？',
      message: `删除自定义分类「${category.name}」。`,
      details: '如果仍有未删除的交易使用这个分类，服务端会拒绝删除。',
      confirmText: '删除分类',
    });
    if (!yes) {
      return;
    }
    setBusy(button, true, '…');
    try {
      await api.deleteCategory(gid, categoryId);
      localCategories = localCategories.filter((item) => String(item.id) !== String(categoryId));
      toast('分类已删除', 'success');
      render();
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
      setBusy(button, false);
    }
  }

  async function saveProfile(event) {
    event.preventDefault();
    const form = event.currentTarget;
    const button = qs('button[type="submit"]', form);
    const nickname = form.elements.nickname.value.trim();
    if (!nickname) {
      toast('昵称不能为空', 'warning');
      return;
    }
    setBusy(button, true, '保存中…');
    try {
      const updated = await api.updateProfile({ nickname, avatar_url: selectedAvatar });
      localUser = { ...localUser, ...updated };
      selectedAvatar = localUser.avatar_url || '';
      onUserUpdated?.(localUser);
      toast('个人资料已更新', 'success');
      if (!onUserUpdated) {
        render();
      }
    } catch (error) {
      toast(friendlyError(error), 'error', 4800);
      setBusy(button, false);
    }
  }

  async function changePassword(event) {
    event.preventDefault();
    const form = event.currentTarget;
    clearFieldErrors(form);
    const oldPassword = form.elements.old_password.value;
    const newPassword = form.elements.new_password.value;
    if (newPassword.length < 8) {
      setFieldError(form, 'new_password', '新密码至少 8 位');
      return;
    }
    if (newPassword !== form.elements.confirm.value) {
      setFieldError(form, 'confirm', '两次输入的新密码不一致');
      return;
    }
    const button = qs('button[type="submit"]', form);
    setBusy(button, true, '更新中…');
    try {
      await api.changePassword(oldPassword, newPassword);
      form.reset();
      toast('密码已更新，其它设备的会话已失效', 'success');
    } catch (error) {
      if (error.code === 1007) {
        setFieldError(form, 'old_password', friendlyError(error));
      }
      toast(friendlyError(error), 'error', 4800);
    } finally {
      setBusy(button, false);
    }
  }

  async function logout(allDevices) {
    const yes = await confirmDialog({
      title: allDevices ? '退出所有设备？' : '退出当前设备？',
      message: allDevices ? '所有设备上的登录凭证都会立即失效。' : '当前浏览器需要重新登录。',
      confirmText: allDevices ? '全部退出' : '退出登录',
    });
    if (!yes) {
      return;
    }
    try {
      await (allDevices ? api.logoutAll() : api.logout());
    } catch {
      // The local session is still cleared if the token already expired.
    }
    clearToken();
    onSignedOut?.();
    toast('已退出登录', 'success');
  }

  async function leaveGroup() {
    const yes = await confirmDialog({
      title: '退出这个组？',
      message: '退出后你将不再看到这本账，历史交易仍会保留。',
      details: '余额必须为 0，否则请先完成结算。',
      confirmText: '退出组',
    });
    if (!yes) {
      return;
    }
    try {
      await api.leaveGroup(gid);
      toast('已退出该组', 'success');
      onGroupDeleted?.();
    } catch (error) {
      toast(friendlyError(error), 'error', 5200);
    }
  }

  async function dissolveGroup() {
    const yes = await confirmDialog({
      title: '解散这个组？',
      message: `解散「${localGroup.name}」后，所有成员都将无法再访问这本账。`,
      details: '数据会软删除。如果仍有未结清余额，下一步会询问是否强制解散。',
      confirmText: '继续解散',
    });
    if (!yes) {
      return;
    }
    try {
      await api.dissolveGroup(gid, false);
      toast('组已解散', 'success');
      onGroupDeleted?.();
    } catch (error) {
      if (error.code === 1015) {
        const force = await confirmDialog({
          title: '仍有未结清余额',
          message: '强制解散会把当前余额快照写入一条系统消息，然后解散账本。',
          details: '这不能替代真实转账。请确认你已经自行处理了结算。',
          confirmText: '强制解散',
        });
        if (!force) {
          return;
        }
        try {
          await api.dissolveGroup(gid, true);
          toast('组已强制解散', 'success');
          onGroupDeleted?.();
        } catch (forceError) {
          toast(friendlyError(forceError), 'error', 5200);
        }
      } else {
        toast(friendlyError(error), 'error', 5200);
      }
    }
  }

  async function deleteAccount() {
    const yes = await confirmDialog({
      title: '注销账号？',
      message: '账号会被软删除，所有会话失效。历史交易保留，但你的名字会显示为已注销用户。',
      details: '如果你仍是某个组的 owner，必须先转让或解散。这个操作不能自行撤销。',
      confirmText: '注销账号',
    });
    if (!yes) {
      return;
    }
    const typed = await new Promise((resolve) => {
      let settled = false;
      const finish = (value) => {
        if (settled) {
          return;
        }
        settled = true;
        resolve(value);
      };
      const modal = openModal({
        title: '确认注销',
        size: 'sm',
        body: html`<div class="form-stack"><p class="muted text-xs">请输入用户名 <strong>${localUser.username}</strong> 以确认。</p><input class="input" data-confirm-username autocomplete="off"></div>`,
        actions: [
          { label: '取消', className: 'button-ghost', onClick: () => { finish(false); modal.close(); } },
          { label: '确认注销', className: 'button-danger', onClick: () => { const value = modal.overlay.querySelector('[data-confirm-username]').value.trim(); finish(value === localUser.username); modal.close(); } },
        ],
        onClose: () => finish(false),
      });
    });
    if (!typed) {
      return;
    }
    try {
      await api.deleteAccount();
      clearToken();
      onSignedOut?.();
      toast('账号已注销', 'success');
    } catch (error) {
      toast(friendlyError(error), 'error', 5200);
    }
  }

  return {
    cleanup() {
      window.removeEventListener('scroll', scheduleSectionSync);
      if (scrollFrame) {
        window.cancelAnimationFrame(scrollFrame);
        scrollFrame = 0;
      }
    },
  };
}
