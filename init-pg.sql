-- PostgreSQL 测试数据库初始化脚本

-- 创建测试数据库
CREATE DATABASE test_db;
\c test_db

-- 创建测试表
CREATE TABLE IF NOT EXISTS users (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(255) UNIQUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS orders (
    id SERIAL PRIMARY KEY,
    user_id INTEGER REFERENCES users(id),
    product_name VARCHAR(255),
    quantity INTEGER DEFAULT 1,
    price DECIMAL(10, 2),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS events (
    id SERIAL PRIMARY KEY,
    event_type VARCHAR(100),
    payload JSONB,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 插入测试数据
INSERT INTO users (name, email) VALUES 
    ('Alice', 'alice@example.com'),
    ('Bob', 'bob@example.com'),
    ('Charlie', 'charlie@example.com')
ON CONFLICT (email) DO NOTHING;

INSERT INTO orders (user_id, product_name, quantity, price) VALUES 
    (1, 'Laptop', 1, 999.99),
    (1, 'Mouse', 2, 29.99),
    (2, 'Keyboard', 1, 79.99);

INSERT INTO events (event_type, payload) VALUES 
    ('user_signup', '{"user_id": 1, "source": "website"}'),
    ('purchase', '{"order_id": 1, "amount": 999.99}');

-- 创建索引
CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);
CREATE INDEX IF NOT EXISTS idx_orders_user_id ON orders(user_id);
CREATE INDEX IF NOT EXISTS idx_events_created_at ON events(created_at);

-- 创建序列示例
CREATE SEQUENCE IF NOT EXISTS test_seq;

-- 创建视图
CREATE OR REPLACE VIEW user_orders_view AS
SELECT u.id, u.name, u.email, COUNT(o.id) as order_count, COALESCE(SUM(o.price * o.quantity), 0) as total_spent
FROM users u
LEFT JOIN orders o ON u.id = o.user_id
GROUP BY u.id, u.name, u.email;

-- 创建函数
CREATE OR REPLACE FUNCTION get_user_stats(user_id INTEGER)
RETURNS TABLE(order_count BIGINT, total_spent NUMERIC) AS $$
BEGIN
    RETURN QUERY
    SELECT COUNT(o.id), COALESCE(SUM(o.price * o.quantity), 0)
    FROM orders o
    WHERE o.user_id = get_user_stats.user_id;
END;
$$ LANGUAGE plpgsql;

-- 创建触发器
CREATE OR REPLACE FUNCTION update_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- 给测试表添加 updated_at 列并创建触发器
ALTER TABLE users ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP;
-- 注意：users 表已创建，这里只演示触发器创建方式

-- PostgreSQL 特有功能演示
-- 创建事件触发器
-- CREATE EVENT TRIGGER ...

-- 创建扩展
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";
CREATE EXTENSION IF NOT EXISTS "pg_trgm";

-- 显示完成信息
\echo 'PostgreSQL test database initialized successfully!'
\echo 'Tables created: users, orders, events'
\echo 'Extensions loaded: uuid-ossp, pg_trgm'
