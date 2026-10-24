const TOKEN_KEY = 'cofinance.token.v1';
let memoryToken = '';

function tabStorage() {
  try {
    return globalThis.sessionStorage || null;
  } catch {
    return null;
  }
}

function dropLegacyPersistentToken() {
  try {
    globalThis.localStorage?.removeItem(TOKEN_KEY);
  } catch {
    // Storage can be unavailable in hardened or private browser modes.
  }
}

export function getToken() {
  const storage = tabStorage();
  if (storage) {
    try {
      const token = storage.getItem(TOKEN_KEY);
      if (token !== null) {
        memoryToken = token;
        return token;
      }
    } catch {
      // Fall through to the in-memory token.
    }
  }
  if (memoryToken) {
    return memoryToken;
  }

  // One-time migration from the old origin-wide token. Tabs opened afterwards
  // start logged out, so each tab can keep an independent account session.
  try {
    const legacy = globalThis.localStorage?.getItem(TOKEN_KEY);
    if (legacy) {
      setToken(legacy);
      return legacy;
    }
  } catch {
    // Ignore storage access failures.
  }
  return '';
}

export function setToken(token) {
  memoryToken = token || '';
  dropLegacyPersistentToken();
  const storage = tabStorage();
  if (!storage) {
    return;
  }
  try {
    if (memoryToken) {
      storage.setItem(TOKEN_KEY, memoryToken);
    } else {
      storage.removeItem(TOKEN_KEY);
    }
  } catch {
    // The in-memory token keeps this tab usable when storage is disabled.
  }
}

export function clearToken() {
  setToken('');
}
