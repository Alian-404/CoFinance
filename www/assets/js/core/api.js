import { getToken, setToken, clearToken } from './session.js';

export class ApiError extends Error {
  constructor(message, { status = 0, code = 0, data = null, aborted = false } = {}) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
    this.data = data;
    this.aborted = aborted;
  }
}

let unauthorizedHandler = null;

export function setUnauthorizedHandler(handler) {
  unauthorizedHandler = handler;
}

function queryString(query) {
  if (!query) {
    return '';
  }
  const params = new URLSearchParams();
  for (const [key, value] of Object.entries(query)) {
    if (value !== undefined && value !== null && value !== '') {
      params.set(key, String(value));
    }
  }
  const encoded = params.toString();
  return encoded ? `?${encoded}` : '';
}

async function request(path, options = {}) {
  const {
    method = 'GET',
    body,
    query,
    auth = true,
    signal,
    timeout = 20_000,
  } = options;
  const controller = new AbortController();
  let timedOut = false;
  const timer = window.setTimeout(() => {
    timedOut = true;
    controller.abort();
  }, timeout);
  const abortFromCaller = () => controller.abort();
  if (signal) {
    if (signal.aborted) {
      controller.abort();
    } else {
      signal.addEventListener('abort', abortFromCaller, { once: true });
    }
  }

  const headers = { Accept: 'application/json' };
  const token = getToken();
  if (auth && token) {
    headers.Authorization = `Bearer ${token}`;
  }
  if (body !== undefined) {
    headers['Content-Type'] = 'application/json';
  }

  try {
    const response = await fetch(`/api${path}${queryString(query)}`, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
      credentials: 'same-origin',
      signal: controller.signal,
    });
    const text = await response.text();
    let payload = null;
    if (text) {
      try {
        payload = JSON.parse(text);
      } catch {
        throw new ApiError('服务器返回了无法解析的响应', { status: response.status });
      }
    }
    if (!response.ok || (payload && Number(payload.code) !== 0)) {
      const error = new ApiError(payload?.msg || `请求失败（HTTP ${response.status}）`, {
        status: response.status,
        code: Number(payload?.code || 0),
        data: payload?.data ?? null,
      });
      if (auth && [1002, 1003].includes(error.code)) {
        clearToken();
        unauthorizedHandler?.(error);
      }
      throw error;
    }
    return payload?.data ?? null;
  } catch (error) {
    if (error instanceof ApiError) {
      throw error;
    }
    if (error?.name === 'AbortError') {
      throw new ApiError(timedOut ? '请求超时，请稍后重试' : '请求已取消', {
        aborted: !timedOut,
        code: timedOut ? 2004 : 0,
      });
    }
    throw new ApiError('网络连接失败，请检查服务是否运行', { code: 2003 });
  } finally {
    window.clearTimeout(timer);
    signal?.removeEventListener('abort', abortFromCaller);
  }
}

function bodyFor(values) {
  return Object.fromEntries(Object.entries(values).filter(([, value]) => value !== undefined));
}

export const api = {
  request,
  register(values) {
    return request('/auth/register', { method: 'POST', body: bodyFor(values), auth: false });
  },
  async login(values) {
    const data = await request('/auth/login', { method: 'POST', body: values, auth: false });
    setToken(data.token);
    return data;
  },
  logout() {
    return request('/auth/logout', { method: 'POST' });
  },
  logoutAll() {
    return request('/auth/logout_all', { method: 'POST' });
  },
  me(options = {}) {
    return request('/users/me', options);
  },
  updateProfile(values) {
    return request('/users/me', { method: 'PUT', body: bodyFor(values) });
  },
  searchUsers(q, options = {}) {
    return request('/users/search', { ...options, query: { q } });
  },
  changePassword(oldPassword, newPassword) {
    return request('/users/me/password', {
      method: 'PUT',
      body: { old_password: oldPassword, new_password: newPassword },
    });
  },
  deleteAccount() {
    return request('/users/me', { method: 'DELETE' });
  },
  groups(options = {}) {
    return request('/groups', options);
  },
  createGroup(name) {
    return request('/groups', { method: 'POST', body: { name } });
  },
  group(gid, options = {}) {
    return request(`/groups/${gid}`, options);
  },
  renameGroup(gid, name) {
    return request(`/groups/${gid}`, { method: 'PUT', body: { name } });
  },
  dissolveGroup(gid, force = false) {
    return request(`/groups/${gid}`, { method: 'DELETE', query: force ? { force: 'true' } : {} });
  },
  members(gid, options = {}) {
    return request(`/groups/${gid}/members`, options);
  },
  inviteMember(gid, username) {
    return request(`/groups/${gid}/members`, { method: 'POST', body: { username } });
  },
  removeMember(gid, uid) {
    return request(`/groups/${gid}/members/${uid}`, { method: 'DELETE' });
  },
  leaveGroup(gid) {
    return request(`/groups/${gid}/members/leave`, { method: 'POST' });
  },
  transferOwner(gid, uid) {
    return request(`/groups/${gid}/members/${uid}/role`, {
      method: 'PUT',
      body: { role: 'owner' },
    });
  },
  invites(options = {}) {
    return request('/invites', options);
  },
  acceptInvite(inviteId) {
    return request(`/invites/${inviteId}/accept`, { method: 'POST' });
  },
  rejectInvite(inviteId) {
    return request(`/invites/${inviteId}/reject`, { method: 'POST' });
  },
  categories(gid, options = {}) {
    return request('/categories', { ...options, query: { gid } });
  },
  createCategory(gid, values) {
    return request(`/groups/${gid}/categories`, { method: 'POST', body: bodyFor(values) });
  },
  deleteCategory(gid, cid) {
    return request(`/groups/${gid}/categories/${cid}`, { method: 'DELETE' });
  },
  transactions(gid, query = {}, options = {}) {
    return request(`/groups/${gid}/transactions`, { ...options, query });
  },
  transaction(gid, tid, options = {}) {
    return request(`/groups/${gid}/transactions/${tid}`, options);
  },
  createTransaction(gid, values) {
    return request(`/groups/${gid}/transactions`, { method: 'POST', body: values });
  },
  updateTransaction(gid, tid, values) {
    return request(`/groups/${gid}/transactions/${tid}`, { method: 'PUT', body: values });
  },
  deleteTransaction(gid, tid) {
    return request(`/groups/${gid}/transactions/${tid}`, { method: 'DELETE' });
  },
  stats(gid, query = {}, options = {}) {
    return request(`/groups/${gid}/stats`, { ...options, query });
  },
  balances(gid, options = {}) {
    return request(`/groups/${gid}/balances`, options);
  },
  suggestSettlements(gid) {
    return request(`/groups/${gid}/settlements/suggest`, { method: 'POST' });
  },
  confirmSettlement(gid, suggestion) {
    return request(`/groups/${gid}/settlements`, {
      method: 'POST',
      body: {
        from_user_id: suggestion.from_user_id,
        to_user_id: suggestion.to_user_id,
        amount_cents: suggestion.amount_cents,
      },
    });
  },
  settlements(gid, options = {}) {
    return request(`/groups/${gid}/settlements`, options);
  },
  messages(gid, query = {}, options = {}) {
    return request(`/groups/${gid}/messages`, { ...options, query });
  },
  sendMessage(gid, content, clientMsgId) {
    return request(`/groups/${gid}/messages`, {
      method: 'POST',
      body: { content, client_msg_id: clientMsgId },
    });
  },
  pollMessages(gid, afterId, timeout = 25, options = {}) {
    return request(`/groups/${gid}/messages/poll`, {
      ...options,
      query: { after_id: afterId, timeout, limit: 100 },
      timeout: (timeout + 5) * 1000,
    });
  },
  markRead(gid, messageId) {
    return request(`/groups/${gid}/messages/read`, {
      method: 'POST',
      body: { last_read_message_id: messageId },
    });
  },
  unread(gid, options = {}) {
    return request(`/groups/${gid}/messages/unread`, options);
  },
  aiAsk(gid, question, options = {}) {
    return request(`/groups/${gid}/ai/ask`, {
      ...options,
      method: 'POST',
      body: { question },
      timeout: 20_000,
    });
  },
  aiSuggestions(gid, options = {}) {
    return request(`/groups/${gid}/ai/suggestions`, { ...options, timeout: 20_000 });
  },
  metrics(options = {}) {
    return request('/metrics', options);
  },
};
