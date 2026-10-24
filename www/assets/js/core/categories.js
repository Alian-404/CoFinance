const icons = {
  restaurant: '🍜',
  commute: '🚇',
  shopping: '🛍',
  home: '🏠',
  entertainment: '🎬',
  medical: '🏥',
  education: '📚',
  income: '💼',
  more: '✦',
};

const nameIcons = {
  餐饮: '🍜',
  交通: '🚇',
  购物: '🛍',
  居住: '🏠',
  娱乐: '🎬',
  医疗: '🏥',
  教育: '📚',
  工资: '💼',
  其他: '✦',
  房租: '🏠',
  旅行: '🧳',
  宠物: '🐾',
  咖啡: '☕',
};

export function categoryEmoji(category) {
  return icons[category?.icon] || nameIcons[category?.name] || '◻';
}

export function categoryTone(index = 0) {
  return ['', 'expense', 'amber', 'blue'][index % 4];
}
