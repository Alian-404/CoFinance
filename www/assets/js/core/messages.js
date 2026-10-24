export function mergeMessages(existing, incoming) {
  const byId = new Map();
  for (const message of [...(existing || []), ...(incoming || [])]) {
    if (!message) {
      continue;
    }
    const key = String(message.id ?? message.client_msg_id ?? '');
    if (!key) {
      continue;
    }
    const previous = byId.get(key);
    if (!previous || Number(message.id || 0) >= Number(previous.id || 0)) {
      byId.set(key, message);
    }
  }
  return [...byId.values()].sort((a, b) => Number(a.id || 0) - Number(b.id || 0));
}

export function overlapAfterId(maxId, overlap = 20) {
  const value = Math.max(0, Number(maxId || 0) - Math.max(0, Number(overlap || 0)));
  return Number.isFinite(value) ? value : 0;
}

export function nextReconnectDelay(attempt) {
  return Math.min(1000 * (2 ** Math.max(0, Number(attempt || 0))), 15_000);
}

export function latestMessageId(messages) {
  return (messages || []).reduce((max, message) => Math.max(max, Number(message?.id || 0)), 0);
}

export function groupMessagesByDay(messages) {
  const groups = [];
  for (const message of messages || []) {
    const raw = String(message.created_at || '');
    const key = raw.slice(0, 10);
    const last = groups[groups.length - 1];
    if (!last || last.key !== key) {
      groups.push({ key, messages: [message] });
    } else {
      last.messages.push(message);
    }
  }
  return groups;
}
