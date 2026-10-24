import { getToken } from './session.js';

export const state = {
  token: getToken(),
  user: null,
  groups: [],
  groupsLoaded: false,
  invites: [],
  currentGroup: null,
  members: [],
  categories: [],
};

export function resetProtectedState() {
  state.user = null;
  state.groups = [];
  state.groupsLoaded = false;
  state.invites = [];
  state.currentGroup = null;
  state.members = [];
  state.categories = [];
}
