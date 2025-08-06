INSERT INTO users (first_name, last_name, email, phone_number)
VALUES
('Ivan', 'Ivanov', 'ivanov@example.com', '+79001234567'),
('Petr', 'Petrov', 'petrov@example.com', '+79111234567'),
('Sergei', 'Sergeev', 'sergeev@example.com', '+79221234567');

INSERT INTO products (name, description, price, stock)
VALUES
('iPhone X', 'new iPhone X by Apple', 50000.00, 100),
('Samsung Galaxy S10', 'New Samsung phone', 40000.00, 150),
('Xiaomi Redmi Note 8 Pro', 'middle class Xiaomi phone', 25000.00, 200);

INSERT INTO orders (user_id, total_amount, status)
VALUES
(1, 50000.00, 'NEW'),
(2, 80000.00, 'PROCESSING'),
(3, 75000.00, 'SHIPPED');


INSERT INTO order_items (order_id, product_id, quantity, unit_price)
VALUES
(1, 1, 1, 50000.00),
(2, 2, 2, 40000.00),
(3, 3, 3, 25000.00);