-- CoFinance schema, generated from docs/design.md section 4.3.
-- Create the database separately, then run: mysql cofinance < sql/schema.sql
-- Safe to run repeatedly; no DROP statements are used.

SET NAMES utf8mb4;

CREATE TABLE IF NOT EXISTS users (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  username      VARCHAR(50)  NOT NULL,
  password_hash VARCHAR(255) NOT NULL,
  nickname      VARCHAR(50)  NOT NULL DEFAULT '',
  avatar_url    VARCHAR(255) NOT NULL DEFAULT '',
  status        TINYINT      NOT NULL DEFAULT 0 COMMENT '0=正常,1=禁用,2=已注销',
  deleted_at    DATETIME     NULL DEFAULT NULL COMMENT '注销时间',
  created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户表';

CREATE TABLE IF NOT EXISTS finance_groups (
  id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  name       VARCHAR(50) NOT NULL,
  owner_id   BIGINT UNSIGNED NOT NULL,
  status     TINYINT NOT NULL DEFAULT 0 COMMENT '0=正常,1=已解散',
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_owner (owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='共同财务组';

CREATE TABLE IF NOT EXISTS group_members (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id      BIGINT UNSIGNED NOT NULL,
  user_id       BIGINT UNSIGNED NOT NULL,
  role          TINYINT NOT NULL DEFAULT 1 COMMENT '0=owner,1=member',
  status        TINYINT NOT NULL DEFAULT 0 COMMENT '0=待接受,1=已加入,2=已拒绝,3=已退出,4=被移除,5=邀请已过期',
  invited_by    BIGINT UNSIGNED NOT NULL DEFAULT 0,
  last_read_message_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  joined_at     DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_group_user (group_id, user_id),
  KEY idx_user_status (user_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='组成员与邀请';

CREATE TABLE IF NOT EXISTS categories (
  id       BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=系统预置',
  name     VARCHAR(50) NOT NULL,
  icon     VARCHAR(50) NOT NULL DEFAULT '',
  sort     INT NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uk_group_name (group_id, name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='消费分类';

INSERT IGNORE INTO categories (group_id, name, icon, sort) VALUES
  (0, '餐饮', 'restaurant', 10),
  (0, '交通', 'commute', 20),
  (0, '购物', 'shopping', 30),
  (0, '居住', 'home', 40),
  (0, '娱乐', 'entertainment', 50),
  (0, '医疗', 'medical', 60),
  (0, '教育', 'education', 70),
  (0, '工资', 'income', 80),
  (0, '其他', 'more', 90);

CREATE TABLE IF NOT EXISTS transactions (
  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id     BIGINT UNSIGNED NOT NULL,
  payer_id     BIGINT UNSIGNED NOT NULL,
  type         TINYINT NOT NULL DEFAULT 0 COMMENT '0=支出,1=收入',
  amount_cents BIGINT NOT NULL COMMENT '金额(分)，恒为正',
  currency     CHAR(3) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT 'CNY',
  category_id  BIGINT UNSIGNED NOT NULL DEFAULT 0,
  note         VARCHAR(255) NOT NULL DEFAULT '',
  split_type   TINYINT NOT NULL DEFAULT 0 COMMENT '0=均分,1=自定义',
  client_token CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL DEFAULT NULL,
  created_by   BIGINT UNSIGNED NOT NULL,
  created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  deleted_at   DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_creator_token (created_by, client_token),
  KEY idx_group_time (group_id, type, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='交易记录';

CREATE TABLE IF NOT EXISTS transaction_participants (
  id             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  transaction_id BIGINT UNSIGNED NOT NULL,
  user_id        BIGINT UNSIGNED NOT NULL,
  share_cents    BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uk_tx_user (transaction_id, user_id),
  KEY idx_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='交易参与人与分摊';

CREATE TABLE IF NOT EXISTS messages (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id      BIGINT UNSIGNED NOT NULL,
  sender_id     BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=系统消息',
  type          TINYINT NOT NULL DEFAULT 0 COMMENT '0=文本,1=系统',
  content       TEXT NOT NULL,
  client_msg_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL DEFAULT NULL,
  created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_sender_client (sender_id, client_msg_id),
  KEY idx_group_id (group_id, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='消息';

CREATE TABLE IF NOT EXISTS sessions (
  token_hash   CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  user_id      BIGINT UNSIGNED NOT NULL,
  created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  expires_at   DATETIME NOT NULL,
  last_seen_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (token_hash),
  KEY idx_user (user_id),
  KEY idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='登录会话';

CREATE TABLE IF NOT EXISTS settlements (
  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id     BIGINT UNSIGNED NOT NULL,
  from_user_id BIGINT UNSIGNED NOT NULL COMMENT '付款方(应付)',
  to_user_id   BIGINT UNSIGNED NOT NULL COMMENT '收款方(应收)',
  amount_cents BIGINT NOT NULL,
  status       TINYINT NOT NULL DEFAULT 0 COMMENT '0=待确认,1=已确认,2=已取消',
  created_by   BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  confirmed_at DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (id),
  KEY idx_group (group_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='结算记录';
